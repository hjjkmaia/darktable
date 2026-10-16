/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/dtdata.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "develop/develop.h"
#include "views/view.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// the format version written into every new file
#define DTDATA_VERSION "1\n"
#define DTDATA_VERSION_ENTRY "version"
// well above any sensor; a mask larger than the image has no use
#define DTDATA_MAX_PIXELS ((size_t)256 << 20)
// color patches are cropped to the edit. Bound their decoded float buffer.
#define DTDATA_MAX_RGBA_PIXELS ((size_t)16 << 20)

// larger than any mask we would store; a bigger entry is damage, not data
#define DTDATA_MAX_ENTRY_SIZE ((int64_t)256 << 20)

// writers rename over the zip, which Windows refuses while a reader has it
// open. not recursive: under the write lock use _zip_list_unlocked, never
// _zip_read or _zip_list
static GRWLock _lock;

typedef struct _scanner_t
{
  char op[32];
  uint32_t kinds;
  dt_dtdata_scan_fn fn;
} _scanner_t;

// filled once at module load, read-only afterwards
static GList *_scanners = NULL;

static const char *_kind_prefix(const dt_dtdata_kind_t kind)
{
  switch(kind)
  {
    case DT_DTDATA_KIND_MASK:  return "mask";
    case DT_DTDATA_KIND_DEPTH: return "depth";
    case DT_DTDATA_KIND_BRUSH: return "brush";
    case DT_DTDATA_KIND_PATCH: return "patch";
  }
  return "data";
}

int dt_dtdata_entry_kind(const char *entry)
{
  if(!entry) return -1;
  for(int k = DT_DTDATA_KIND_MASK; k <= DT_DTDATA_KIND_PATCH; k++)
  {
    const char *prefix = _kind_prefix(k);
    const size_t n = strlen(prefix);
    if(!strncmp(entry, prefix, n) && entry[n] == '-') return k;
  }
  return -1;
}

gboolean dt_dtdata_enabled(void)
{
  return dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER;
}

gboolean dt_dtdata_file_source_sha256(const char *path,
                                      char out[DT_DTDATA_SOURCE_SHA256_LEN])
{
  if(!out) return FALSE;
  out[0] = '\0';
  if(!path || !path[0]) return FALSE;

  FILE *f = g_fopen(path, "rb");
  if(!f) return FALSE;
  struct stat before = { 0 }, after = { 0 }, path_after = { 0 };
  gboolean ok = fstat(fileno(f), &before) == 0 && S_ISREG(before.st_mode);
  GChecksum *checksum = ok ? g_checksum_new(G_CHECKSUM_SHA256) : NULL;
  ok = ok && checksum != NULL;
  guchar buffer[64 * 1024];
  while(ok)
  {
    const size_t n = fread(buffer, 1, sizeof(buffer), f);
    if(n) g_checksum_update(checksum, buffer, n);
    if(n < sizeof(buffer))
    {
      if(ferror(f)) ok = FALSE;
      break;
    }
  }
  if(ok)
    ok = fstat(fileno(f), &after) == 0
         && g_stat(path, &path_after) == 0
         && before.st_dev == after.st_dev
         && before.st_ino == after.st_ino
         && before.st_size == after.st_size
         && before.st_mtime == after.st_mtime
         && before.st_ctime == after.st_ctime
         && before.st_dev == path_after.st_dev
         && before.st_ino == path_after.st_ino
         && before.st_size == path_after.st_size
         && before.st_mtime == path_after.st_mtime
         && before.st_ctime == path_after.st_ctime;
#if defined(__APPLE__)
  if(ok)
    ok = before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec
         && before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec
         && before.st_mtimespec.tv_nsec == path_after.st_mtimespec.tv_nsec
         && before.st_ctimespec.tv_nsec == path_after.st_ctimespec.tv_nsec;
#elif defined(__linux__)
  if(ok)
    ok = before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
         && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec
         && before.st_mtim.tv_nsec == path_after.st_mtim.tv_nsec
         && before.st_ctim.tv_nsec == path_after.st_ctim.tv_nsec;
#endif
  if(fclose(f) != 0) ok = FALSE;
  if(ok)
    g_strlcpy(out, g_checksum_get_string(checksum), DT_DTDATA_SOURCE_SHA256_LEN);
  if(checksum) g_checksum_free(checksum);
  return ok;
}

gboolean dt_dtdata_source_sha256(const dt_imgid_t imgid,
                                 char out[DT_DTDATA_SOURCE_SHA256_LEN])
{
  if(!out) return FALSE;
  out[0] = '\0';
  if(!dt_is_valid_imgid(imgid)) return FALSE;
  char path[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, path, sizeof(path), &from_cache);
  return dt_dtdata_file_source_sha256(path, out);
}

void dt_dtdata_path_for_image(const char *versioned_image_path,
                              char *path,
                              const size_t len)
{
  snprintf(path, len, "%s%s", versioned_image_path, DT_DTDATA_EXT);
}

// empty when the image is unknown, so no caller ends up on a bare ".dtdata"
void dt_dtdata_path(const dt_imgid_t imgid, char *path, const size_t len)
{
  path[0] = 0;
  char image[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, image, sizeof(image), &from_cache);
  if(!image[0]) return;
  // same rule as dt_image_write_sidecar_file(): beside the original when
  // it is reachable, else beside the local copy
  if(!g_file_test(image, G_FILE_TEST_EXISTS))
  {
    from_cache = TRUE;
    dt_image_full_path(imgid, image, sizeof(image), &from_cache);
  }
  dt_image_path_append_version(imgid, image, sizeof(image));
  dt_dtdata_path_for_image(image, path, len);
}

// --- PNG in memory ---

static void _png_write_cb(png_structp png, png_bytep data, png_size_t len)
{
  g_byte_array_append((GByteArray *)png_get_io_ptr(png), data, len);
}

static void _png_flush_cb(png_structp png)
{
}

static GBytes *_encode_gray_png(const float *mask,
                                const int width,
                                const int height,
                                const int bpc)
{
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_write_struct(&png, NULL);
    return NULL;
  }

  GByteArray *out = g_byte_array_new();
  // 16 bit needs two bytes per pixel; sized for the larger case
  uint8_t *row = g_malloc((size_t)width * 2);

  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    g_byte_array_unref(out);
    g_free(row);
    return NULL;
  }

  png_set_write_fn(png, out, _png_write_cb, _png_flush_cb);
  png_set_IHDR(png, info, width, height, bpc, PNG_COLOR_TYPE_GRAY,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  // rows are handed over in host byte order
  if(bpc == 16) png_set_swap(png);

  for(int y = 0; y < height; y++)
  {
    const float *in = mask + (size_t)y * width;
    if(bpc == 16)
    {
      uint16_t *r = (uint16_t *)row;
      for(int x = 0; x < width; x++) r[x] = (uint16_t)(CLIP(in[x]) * 65535.0f + 0.5f);
    }
    else
    {
      for(int x = 0; x < width; x++) row[x] = (uint8_t)(CLIP(in[x]) * 255.0f + 0.5f);
    }
    png_write_row(png, row);
  }
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  g_free(row);
  return g_byte_array_free_to_bytes(out);
}

static GBytes *_encode_rgba_png(const float *rgba,
                                const int width,
                                const int height,
                                const int bpc)
{
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_write_struct(&png, NULL);
    return NULL;
  }

  GByteArray *out = g_byte_array_new();
  uint8_t *row = g_malloc((size_t)width * 4 * (bpc / 8));
  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    g_byte_array_unref(out);
    g_free(row);
    return NULL;
  }

  png_set_write_fn(png, out, _png_write_cb, _png_flush_cb);
  png_set_IHDR(png, info, width, height, bpc, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  if(bpc == 16) png_set_swap(png);
#endif

  for(int y = 0; y < height; y++)
  {
    const float *in = rgba + (size_t)y * width * 4;
    if(bpc == 16)
    {
      uint16_t *r = (uint16_t *)row;
      for(size_t x = 0; x < (size_t)width * 4; x++)
        r[x] = (uint16_t)(CLIP(in[x]) * 65535.0f + 0.5f);
    }
    else
    {
      for(size_t x = 0; x < (size_t)width * 4; x++)
        row[x] = (uint8_t)(CLIP(in[x]) * 255.0f + 0.5f);
    }
    png_write_row(png, row);
  }
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  g_free(row);
  return g_byte_array_free_to_bytes(out);
}

typedef struct _png_mem_t
{
  const uint8_t *data;
  size_t len;
  size_t pos;
} _png_mem_t;

static void _png_read_cb(png_structp png, png_bytep out, png_size_t len)
{
  _png_mem_t *m = png_get_io_ptr(png);
  if(m->pos + len > m->len) png_error(png, "truncated png");
  memcpy(out, m->data + m->pos, len);
  m->pos += len;
}

// any PNG comes back as a single gray float plane in [0,1]
static float *_decode_gray_png(const uint8_t *data,
                               const size_t len,
                               int *width,
                               int *height)
{
  if(len < 8 || png_sig_cmp(data, 0, 8)) return NULL;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_read_struct(&png, NULL, NULL);
    return NULL;
  }

  _png_mem_t mem = { data, len, 0 };
  // libpng reports errors with a longjmp to png_jmpbuf; these are assigned
  // after setjmp and freed in that branch, so volatile keeps their values
  uint8_t *volatile rows = NULL;
  float *volatile mask = NULL;

  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, NULL);
    dt_free_align(rows);
    dt_free_align(mask);
    return NULL;
  }

  png_set_read_fn(png, &mem, _png_read_cb);
  png_set_user_limits(png, 32768, 32768);
  png_read_info(png, info);

  png_uint_32 w = 0, h = 0;
  int bit_depth = 0, color_type = 0;
  png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

  if(color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if(png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if(color_type & PNG_COLOR_MASK_COLOR) png_set_rgb_to_gray_fixed(png, 1, -1, -1);
  png_set_strip_alpha(png);
  if(bit_depth == 16) png_set_swap(png);
  png_read_update_info(png, info);

  // the zip size cap sees compressed bytes: a tiny entry can still declare
  // a huge image, so bound the pixels before allocating for them
  if((size_t)w * h > DTDATA_MAX_PIXELS) png_error(png, "image too large");

  const size_t rowbytes = png_get_rowbytes(png, info);
  const int out_depth = png_get_bit_depth(png, info);
  rows = dt_alloc_aligned(rowbytes);
  mask = dt_alloc_align_float((size_t)w * h);
  if(!rows || !mask) png_error(png, "out of memory");

  // one row at a time straight into the mask, no second full-size buffer
  const float norm = out_depth == 16 ? 1.0f / 65535.0f : 1.0f / 255.0f;
  for(png_uint_32 y = 0; y < h; y++)
  {
    png_read_row(png, rows, NULL);
    float *out = mask + (size_t)y * w;
    if(out_depth == 16)
      for(png_uint_32 x = 0; x < w; x++) out[x] = ((const uint16_t *)rows)[x] * norm;
    else
      for(png_uint_32 x = 0; x < w; x++) out[x] = rows[x] * norm;
  }
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  dt_free_align(rows);
  *width = (int)w;
  *height = (int)h;
  return mask;
}

static float *_decode_rgba_png(const uint8_t *data,
                               const size_t len,
                               int *width,
                               int *height,
                               int *bpc)
{
  if(len < 8 || png_sig_cmp(data, 0, 8)) return NULL;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_read_struct(&png, NULL, NULL);
    return NULL;
  }

  _png_mem_t mem = { data, len, 0 };
  uint8_t *volatile row = NULL;
  float *volatile rgba = NULL;
  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, NULL);
    dt_free_align(row);
    dt_free_align(rgba);
    return NULL;
  }

  png_set_read_fn(png, &mem, _png_read_cb);
  png_set_user_limits(png, 32768, 32768);
  png_read_info(png, info);

  png_uint_32 w = 0, h = 0;
  int depth = 0, color = 0;
  png_get_IHDR(png, info, &w, &h, &depth, &color, NULL, NULL, NULL);
  if(color != PNG_COLOR_TYPE_RGBA || (depth != 8 && depth != 16)
     || (size_t)w * h > DTDATA_MAX_RGBA_PIXELS)
  {
    png_destroy_read_struct(&png, &info, NULL);
    return NULL;
  }
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  if(depth == 16) png_set_swap(png);
#endif
  png_read_update_info(png, info);
  const size_t rowbytes = png_get_rowbytes(png, info);
  if(rowbytes != (size_t)w * 4 * (depth / 8))
  {
    png_destroy_read_struct(&png, &info, NULL);
    return NULL;
  }

  row = dt_alloc_aligned(rowbytes);
  rgba = dt_alloc_align_float((size_t)w * h * 4);
  if(!row || !rgba) png_error(png, "out of memory");

  const float norm = depth == 16 ? 1.0f / 65535.0f : 1.0f / 255.0f;
  for(png_uint_32 y = 0; y < h; y++)
  {
    png_read_row(png, row, NULL);
    float *out = rgba + (size_t)y * w * 4;
    if(depth == 16)
      for(size_t x = 0; x < (size_t)w * 4; x++) out[x] = ((const uint16_t *)row)[x] * norm;
    else
      for(size_t x = 0; x < (size_t)w * 4; x++) out[x] = row[x] * norm;
  }
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  dt_free_align(row);
  *width = (int)w;
  *height = (int)h;
  *bpc = depth;
  return rgba;
}

// --- zip ---

static gboolean _write_entry(struct archive *w,
                             const char *name,
                             const void *data,
                             const size_t len)
{
  struct archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, name);
  archive_entry_set_size(e, len);
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, 0644);
  gboolean ok = archive_write_header(w, e) == ARCHIVE_OK
                && (len == 0 || archive_write_data(w, data, len) == (ssize_t)len);
  archive_entry_free(e);
  return ok;
}

// libarchive leaves the error string unset on a plain short read
static const char *_archive_err(struct archive *a)
{
  const char *s = archive_error_string(a);
  return s ? s : "unexpected end of file";
}

// libarchive's own open takes the path in the ANSI code page on Windows,
// g_fopen keeps it UTF-8. the FILE outlives the archive: caller closes it
static FILE *_zip_open_read(struct archive *r, const char *path)
{
  FILE *f = g_fopen(path, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, g_strerror(errno));
    return NULL;
  }
  archive_read_support_format_zip(r);
  if(archive_read_open_FILE(r, f) != ARCHIVE_OK)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
    fclose(f);
    return NULL;
  }
  return f;
}

// a header loop keeps going on ARCHIVE_WARN; anything else but EOF at its
// end means the zip could not be read through
static gboolean _next_header(struct archive *r, struct archive_entry **e, int *rc)
{
  *rc = archive_read_next_header(r, e);
  return *rc == ARCHIVE_OK || *rc == ARCHIVE_WARN;
}

// the current entry's bytes into buf. FALSE when the data does not end
// cleanly or exceeds the size limit, so a cut entry is never copied short
static gboolean _read_entry_data(struct archive *r,
                                 struct archive_entry *e,
                                 GByteArray *buf)
{
  const char *n = archive_entry_pathname(e);
  if(archive_entry_size_is_set(e) && archive_entry_size(e) > DTDATA_MAX_ENTRY_SIZE)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' is larger than 256 MB, refused", n);
    return FALSE;
  }
  const void *blk = NULL;
  size_t sz = 0;
  int64_t off = 0;
  int rc;
  gboolean warned = FALSE;
  while((rc = archive_read_data_block(r, &blk, &sz, &off)) == ARCHIVE_OK || rc == ARCHIVE_WARN)
  {
    if(rc == ARCHIVE_WARN) warned = TRUE;
    if((int64_t)buf->len + (int64_t)sz > DTDATA_MAX_ENTRY_SIZE)
    {
      dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' is larger than 256 MB, refused", n);
      return FALSE;
    }
    g_byte_array_append(buf, blk, sz);
  }
  if(rc != ARCHIVE_EOF)
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read entry '%s': %s", n, _archive_err(r));
  return rc == ARCHIVE_EOF && !warned;
}

static uint16_t _zip_u16(const unsigned char *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t _zip_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// libarchive streams local ZIP headers, and may report EOF after a damaged
// local header even when the central directory survived. Verify that the
// central directory is structurally complete and names exactly the entries
// the streaming reader saw before replacing any sidecar.
static gboolean _zip_central_matches(const char *path, GHashTable *streamed)
{
  FILE *f = g_fopen(path, "rb");
  if(!f) return FALSE;
  gboolean valid = FALSE;
  if(fseek(f, 0, SEEK_END) == 0)
  {
    const long size = ftell(f);
    if(size >= 22)
    {
      const size_t tail = MIN((size_t)size, (size_t)22 + 65535);
      unsigned char *buf = g_malloc(tail);
      if(fseek(f, size - (long)tail, SEEK_SET) == 0
         && fread(buf, 1, tail, f) == tail)
      {
        for(size_t i = tail - 22; ; i--)
        {
          if(!memcmp(buf + i, "PK\005\006", 4))
          {
            const size_t comment = _zip_u16(buf + i + 20);
            if(i + 22 + comment == tail)
            {
              const uint16_t count = _zip_u16(buf + i + 10);
              const uint32_t cd_size = _zip_u32(buf + i + 12);
              const uint32_t cd_offset = _zip_u32(buf + i + 16);
              const size_t eocd_at = (size_t)size - tail + i;
              // Sidecars are ordinary single-disk ZIPs. Reject ZIP64 rather
              // than accepting a directory whose real extent is unknown.
              if(_zip_u16(buf + i + 4) == 0 && _zip_u16(buf + i + 6) == 0
                 && _zip_u16(buf + i + 8) == count
                 && count != UINT16_MAX && cd_size != UINT32_MAX
                 && cd_offset != UINT32_MAX
                 && (uint64_t)cd_offset + cd_size == eocd_at
                 && (!streamed || g_hash_table_size(streamed) == count)
                 && fseek(f, cd_offset, SEEK_SET) == 0)
              {
                GHashTable *central = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                valid = TRUE;
                for(uint16_t j = 0; valid && j < count; j++)
                {
                  unsigned char header[46];
                  valid = fread(header, 1, sizeof(header), f) == sizeof(header)
                          && !memcmp(header, "PK\001\002", 4);
                  if(!valid) break;
                  const uint16_t nlen = _zip_u16(header + 28);
                  const uint16_t extra = _zip_u16(header + 30);
                  const uint16_t note = _zip_u16(header + 32);
                  const uint32_t local = _zip_u32(header + 42);
                  if(!nlen || local >= cd_offset) { valid = FALSE; break; }
                  char *name = g_malloc((size_t)nlen + 1);
                  valid = fread(name, 1, nlen, f) == nlen;
                  name[nlen] = '\0';
                  valid = valid && !memchr(name, '\0', nlen)
                          && (!streamed || g_hash_table_contains(streamed, name))
                          && !g_hash_table_contains(central, name)
                          && fseek(f, (long)extra + note, SEEK_CUR) == 0;
                  if(valid) g_hash_table_add(central, name);
                  else g_free(name);
                }
                valid = valid && ftell(f) == (long)((uint64_t)cd_offset + cd_size);
                g_hash_table_unref(central);
                if(valid) break;
              }
            }
          }
          if(i == 0) break;
        }
      }
      g_free(buf);
    }
  }
  if(fclose(f) != 0) valid = FALSE;
  return valid;
}

static gboolean _entry_matches_bytes(const char *name,
                                     const void *bytes,
                                     const size_t len)
{
  if(!name || !name[0] || strchr(name, '/') || strchr(name, '\\')) return FALSE;
  if(!strcmp(name, DTDATA_VERSION_ENTRY))
    return len == strlen(DTDATA_VERSION)
           && !memcmp(bytes, DTDATA_VERSION, len);
  const int kind = dt_dtdata_entry_kind(name);
  if(kind < 0) return TRUE; // preserve entries from a newer producer
  gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA1, bytes, len);
  gchar *expected = g_strdup_printf("%s-%s.png", _kind_prefix(kind), sha);
  const gboolean valid = !strcmp(name, expected);
  g_free(expected);
  g_free(sha);
  return valid;
}

// copy the zip to a temp file without the entries in drop, adding name if
// it is not there yet, then rename it over. name may be NULL to only drop
static gboolean _zip_rewrite(const char *path,
                             const GList *drop,
                             const char *name,
                             const void *data,
                             const size_t len)
{
  gboolean ok = FALSE;
  if(g_file_test(path, G_FILE_TEST_EXISTS) && !_zip_central_matches(path, NULL))
    return FALSE;
  gchar *tmp = g_strdup_printf("%s.tmp", path);

  FILE *wf = g_fopen(tmp, "wb");
  if(!wf)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot write '%s': %s", tmp, g_strerror(errno));
    g_free(tmp);
    return FALSE;
  }
  struct archive *w = archive_write_new();
  archive_write_set_format_zip(w);
  // zip is not a tape format: no zero padding to the 10 KiB block
  archive_write_set_bytes_in_last_block(w, 1);
  if(archive_write_open_FILE(w, wf) != ARCHIVE_OK)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot write '%s': %s", tmp, _archive_err(w));
    archive_write_free(w);
    fclose(wf);
    g_unlink(tmp);
    g_free(tmp);
    return FALSE;
  }

  gboolean have_version = FALSE, have_entry = FALSE, copy_ok = TRUE;
  if(g_file_test(path, G_FILE_TEST_EXISTS))
  {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    struct archive *r = archive_read_new();
    FILE *rf = _zip_open_read(r, path);
    if(rf)
    {
      struct archive_entry *e = NULL;
      int rc = ARCHIVE_EOF;
      while(copy_ok && _next_header(r, &e, &rc))
      {
        const char *n = archive_entry_pathname(e);
        if(rc != ARCHIVE_OK || !n || g_hash_table_contains(seen, n))
        {
          copy_ok = FALSE;
          break;
        }
        g_hash_table_add(seen, g_strdup(n));
        if(!strcmp(n, DTDATA_VERSION_ENTRY)) have_version = TRUE;
        if(name && !strcmp(n, name)) have_entry = TRUE;
        if(g_list_find_custom((GList *)drop, n, (GCompareFunc)g_strcmp0))
        {
          copy_ok = archive_read_data_skip(r) == ARCHIVE_OK;
          continue;
        }

        GByteArray *buf = g_byte_array_new();
        copy_ok = _read_entry_data(r, e, buf)
                  && _entry_matches_bytes(n, buf->data, buf->len)
                  && _write_entry(w, n, buf->data, buf->len);
        g_byte_array_unref(buf);
      }
      if(copy_ok && rc != ARCHIVE_EOF)
      {
        dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
        copy_ok = FALSE;
      }
      if(archive_read_close(r) != ARCHIVE_OK) copy_ok = FALSE;
    }
    else
      copy_ok = FALSE;
    archive_read_free(r);
    if(rf && fclose(rf) != 0) copy_ok = FALSE;
    if(copy_ok && !_zip_central_matches(path, seen)) copy_ok = FALSE;
    g_hash_table_unref(seen);
  }

  if(copy_ok && !have_version)
    copy_ok = _write_entry(w, DTDATA_VERSION_ENTRY, DTDATA_VERSION, strlen(DTDATA_VERSION));
  if(copy_ok && name && !have_entry)
    copy_ok = _write_entry(w, name, data, len);

  ok = archive_write_close(w) == ARCHIVE_OK && copy_ok;
  archive_write_free(w);
  // the FILE buffer is flushed here, so it must be closed before the rename
  if(fclose(wf) != 0) ok = FALSE;

  if(ok && g_rename(tmp, path) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot replace '%s'", path);
    ok = FALSE;
  }
  if(!ok) g_unlink(tmp);
  g_free(tmp);
  return ok;
}

// the entry's bytes, or NULL if the file or entry is missing or damaged
static GBytes *_zip_read(const char *path, const char *name)
{
  GBytes *result = NULL;
  gboolean found = FALSE;
  g_rw_lock_reader_lock(&_lock);
  struct archive *r = archive_read_new();
  FILE *f = _zip_open_read(r, path);
  if(f)
  {
    struct archive_entry *e = NULL;
    int rc = ARCHIVE_EOF;
    while(!found && _next_header(r, &e, &rc))
    {
      const char *n = archive_entry_pathname(e);
      if(!n || strcmp(n, name)) continue;
      found = TRUE;
      GByteArray *buf = g_byte_array_new();
      if(_read_entry_data(r, e, buf))
        result = g_byte_array_free_to_bytes(buf);
      else
        g_byte_array_unref(buf);
    }
    if(!found && rc != ARCHIVE_EOF)
      dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
    archive_read_close(r);
  }
  archive_read_free(r);
  if(f) fclose(f);
  g_rw_lock_reader_unlock(&_lock);
  return result;
}

// entry names without "version", sorted. NULL with *ok FALSE when the zip
// could not be read to its end, so a damaged file does not look merely short
// caller holds the lock, either side
static GList *_zip_list_unlocked(const char *path, gboolean *ok)
{
  GList *list = NULL;
  *ok = FALSE;
  struct archive *r = archive_read_new();
  FILE *f = _zip_open_read(r, path);
  if(f)
  {
    struct archive_entry *e = NULL;
    int rc = ARCHIVE_EOF;
    while(_next_header(r, &e, &rc))
    {
      const char *n = archive_entry_pathname(e);
      if(n && strcmp(n, DTDATA_VERSION_ENTRY)) list = g_list_prepend(list, g_strdup(n));
    }
    *ok = rc == ARCHIVE_EOF;
    if(!*ok)
      dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
    archive_read_close(r);
  }
  archive_read_free(r);
  if(f) fclose(f);
  if(!*ok)
  {
    g_list_free_full(list, g_free);
    return NULL;
  }
  return g_list_sort(list, (GCompareFunc)g_strcmp0);
}

static GList *_zip_list(const char *path, gboolean *ok)
{
  g_rw_lock_reader_lock(&_lock);
  GList *list = _zip_list_unlocked(path, ok);
  g_rw_lock_reader_unlock(&_lock);
  return list;
}

GList *dt_dtdata_file_list_entries(const char *path)
{
  if(!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return NULL;
  gboolean ok = FALSE;
  return _zip_list(path, &ok);
}

GList *dt_dtdata_find_all(const char *image_path)
{
  GList *list = NULL;
  if(!image_path || !*image_path) return NULL;
  gchar *dir = g_path_get_dirname(image_path);
  gchar *base = g_path_get_basename(image_path);
  const char *dot = strrchr(base, '.');
  const size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
  const char *ext = dot ? dot : "";
  gchar *tail = g_strconcat(ext, DT_DTDATA_EXT, NULL);
  const size_t tail_len = strlen(tail);

  GDir *d = g_dir_open(dir, 0, NULL);
  const char *name;
  while(d && (name = g_dir_read_name(d)))
  {
    const size_t len = strlen(name);
    if(len < stem_len + tail_len || strncmp(name, base, stem_len)
       || strcmp(name + len - tail_len, tail))
      continue;
    // between the stem and the tail: nothing, or "_" and digits only
    const char *v = name + stem_len;
    const char *vend = name + len - tail_len;
    if(v != vend)
    {
      if(*v != '_' || v + 1 == vend) continue;
      gboolean digits = TRUE;
      for(const char *c = v + 1; c < vend; c++)
        if(!g_ascii_isdigit(*c)) digits = FALSE;
      if(!digits) continue;
    }
    list = g_list_prepend(list, g_build_filename(dir, name, NULL));
  }
  if(d) g_dir_close(d);
  g_free(tail);
  g_free(base);
  g_free(dir);
  return g_list_sort(list, (GCompareFunc)g_strcmp0);
}

// --- entries ---

// "<kind>-<sha1>.png": the name is the checksum, so a damaged or swapped
// file is caught on read without any manifest
static void _entry_name(const dt_dtdata_kind_t kind,
                        GBytes *png,
                        char *name,
                        const size_t len)
{
  gchar *sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, png);
  snprintf(name, len, "%s-%s.png", _kind_prefix(kind), sha);
  g_free(sha);
}

gboolean dt_dtdata_file_write_gray(const char *path,
                                   const dt_dtdata_kind_t kind,
                                   const dt_dtdata_origin_t origin,
                                   const char *producer,
                                   const float *mask,
                                   const int width,
                                   const int height,
                                   const int bpc,
                                   dt_dtdata_ref_t *ref)
{
  if(!path || !mask || width <= 0 || height <= 0 || (bpc != 8 && bpc != 16)) return FALSE;

  GBytes *png = _encode_gray_png(mask, width, height, bpc);
  if(!png)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot encode %dx%d mask", width, height);
    return FALSE;
  }

  char name[DT_DTDATA_ENTRY_LEN] = { 0 };
  _entry_name(kind, png, name, sizeof(name));

  gsize len = 0;
  const void *data = g_bytes_get_data(png, &len);
  g_rw_lock_writer_lock(&_lock);
  const gboolean ok = _zip_rewrite(path, NULL, name, data, len);
  g_rw_lock_writer_unlock(&_lock);
  g_bytes_unref(png);
  if(!ok) return FALSE;

  if(ref)
  {
    memset(ref, 0, sizeof(*ref));
    g_strlcpy(ref->entry, name, sizeof(ref->entry));
    ref->kind = kind;
    ref->origin = origin;
    ref->width = width;
    ref->height = height;
    ref->bpc = bpc;
    if(producer) g_strlcpy(ref->producer, producer, sizeof(ref->producer));
  }
  dt_print(DT_DEBUG_MASKS, "[dtdata] wrote %s (%dx%d, %d bit) to '%s'",
           name, width, height, bpc, path);
  return TRUE;
}

gboolean dt_dtdata_file_write_rgba(const char *path,
                                   const char *producer,
                                   const float *rgba,
                                   const int width,
                                   const int height,
                                   const int bpc,
                                   dt_dtdata_ref_t *ref)
{
  if(!path || !rgba || width <= 0 || height <= 0
     || width > 32768 || height > 32768
     || (size_t)width * height > DTDATA_MAX_RGBA_PIXELS
     || (bpc != 8 && bpc != 16)) return FALSE;

  GBytes *png = _encode_rgba_png(rgba, width, height, bpc);
  if(!png) return FALSE;
  char name[DT_DTDATA_ENTRY_LEN] = { 0 };
  _entry_name(DT_DTDATA_KIND_PATCH, png, name, sizeof(name));

  gsize len = 0;
  const void *data = g_bytes_get_data(png, &len);
  if(len > DTDATA_MAX_ENTRY_SIZE)
  {
    g_bytes_unref(png);
    return FALSE;
  }
  g_rw_lock_writer_lock(&_lock);
  const gboolean ok = _zip_rewrite(path, NULL, name, data, len);
  g_rw_lock_writer_unlock(&_lock);
  g_bytes_unref(png);
  if(!ok) return FALSE;

  if(ref)
  {
    memset(ref, 0, sizeof(*ref));
    g_strlcpy(ref->entry, name, sizeof(ref->entry));
    ref->kind = DT_DTDATA_KIND_PATCH;
    ref->origin = DT_DTDATA_ORIGIN_AUTHORITATIVE;
    ref->width = width;
    ref->height = height;
    ref->bpc = bpc;
    if(producer) g_strlcpy(ref->producer, producer, sizeof(ref->producer));
  }
  dt_print(DT_DEBUG_MASKS, "[dtdata] wrote %s (%dx%d, %d bit) to '%s'",
           name, width, height, bpc, path);
  return TRUE;
}

static GBytes *_read_checked_png(const char *path, const dt_dtdata_ref_t *ref)
{
  if(!path || !ref || !ref->entry[0]
     || !memchr(ref->entry, '\0', sizeof(ref->entry))) return NULL;

  GBytes *png = _zip_read(path, ref->entry);
  if(!png)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' not found in '%s'", ref->entry, path);
    return NULL;
  }

  // the hash sits between the kind prefix and ".png"
  const char *dash = strchr(ref->entry, '-');
  const char *dot = strrchr(ref->entry, '.');
  gchar *sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, png);
  const gboolean intact = dash && dot && dot > dash + 1
                          && strlen(sha) == (size_t)(dot - dash - 1)
                          && !strncmp(sha, dash + 1, dot - dash - 1);
  g_free(sha);
  if(!intact)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' in '%s' does not match its checksum",
             ref->entry, path);
    g_bytes_unref(png);
    return NULL;
  }

  return png;
}

float *dt_dtdata_file_read_gray(const char *path,
                                const dt_dtdata_ref_t *ref,
                                int *width,
                                int *height)
{
  *width = *height = 0;
  GBytes *png = _read_checked_png(path, ref);
  if(!png) return NULL;

  gsize len = 0;
  const void *data = g_bytes_get_data(png, &len);
  float *mask = _decode_gray_png(data, len, width, height);
  g_bytes_unref(png);
  if(!mask)
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' in '%s' is not a readable PNG",
             ref->entry, path);
  return mask;
}

float *dt_dtdata_file_read_rgba(const char *path,
                                const dt_dtdata_ref_t *ref,
                                int *width,
                                int *height)
{
  *width = *height = 0;
  if(!ref || ref->kind != DT_DTDATA_KIND_PATCH
     || ref->origin != DT_DTDATA_ORIGIN_AUTHORITATIVE
     || !memchr(ref->entry, '\0', sizeof(ref->entry))
     || dt_dtdata_entry_kind(ref->entry) != DT_DTDATA_KIND_PATCH) return NULL;
  GBytes *png = _read_checked_png(path, ref);
  if(!png) return NULL;

  gsize len = 0;
  const void *data = g_bytes_get_data(png, &len);
  int bpc = 0;
  float *rgba = _decode_rgba_png(data, len, width, height, &bpc);
  g_bytes_unref(png);
  if(!rgba || *width != ref->width || *height != ref->height || bpc != ref->bpc)
  {
    dt_free_align(rgba);
    *width = *height = 0;
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' in '%s' is not a matching RGBA patch",
             ref->entry, path);
    return NULL;
  }
  return rgba;
}

static gboolean _zip_copy_validated_entries(const char *path,
                                            struct archive *w,
                                            GHashTable *written)
{
  struct archive *r = archive_read_new();
  FILE *f = _zip_open_read(r, path);
  if(!f)
  {
    archive_read_free(r);
    return FALSE;
  }
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean ok = TRUE, have_version = FALSE;
  struct archive_entry *e = NULL;
  int rc = ARCHIVE_EOF;
  while(ok && _next_header(r, &e, &rc))
  {
    const char *name = archive_entry_pathname(e);
    if(rc != ARCHIVE_OK || !name || g_hash_table_contains(seen, name))
    {
      ok = FALSE;
      break;
    }
    g_hash_table_add(seen, g_strdup(name));
    GByteArray *data = g_byte_array_new();
    ok = _read_entry_data(r, e, data)
         && _entry_matches_bytes(name, data->data, data->len);
    if(ok && !strcmp(name, DTDATA_VERSION_ENTRY)) have_version = TRUE;
    if(ok && w && !g_hash_table_contains(written, name))
    {
      ok = _write_entry(w, name, data->data, data->len);
      if(ok) g_hash_table_add(written, g_strdup(name));
    }
    g_byte_array_unref(data);
  }
  if(rc != ARCHIVE_EOF || !have_version) ok = FALSE;
  if(archive_read_close(r) != ARCHIVE_OK) ok = FALSE;
  archive_read_free(r);
  if(fclose(f) != 0) ok = FALSE;
  if(ok && !_zip_central_matches(path, seen)) ok = FALSE;
  g_hash_table_unref(seen);
  return ok;
}

gboolean dt_dtdata_file_merge(const char *src_path, const char *dst_path)
{
  if(!src_path || !dst_path) return FALSE;
  if(!g_file_test(src_path, G_FILE_TEST_EXISTS)) return TRUE;

  // Build the complete, validated union beside the destination and rename
  // once. A failed later entry must not leave an earlier one half-merged.
  g_rw_lock_writer_lock(&_lock);
  gchar *tmp = g_strdup_printf("%s.merge-XXXXXX", dst_path);
  const int fd = g_mkstemp(tmp);
  FILE *wf = fd >= 0 ? fdopen(fd, "wb") : NULL;
  if(fd >= 0 && !wf) g_close(fd, NULL);
  gboolean ok = wf != NULL;
  struct archive *w = ok ? archive_write_new() : NULL;
  if(ok)
  {
    archive_write_set_format_zip(w);
    archive_write_set_bytes_in_last_block(w, 1);
    ok = archive_write_open_FILE(w, wf) == ARCHIVE_OK;
  }
  GHashTable *written = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if(ok && g_file_test(dst_path, G_FILE_TEST_EXISTS))
    ok = _zip_copy_validated_entries(dst_path, w, written);
  if(ok) ok = _zip_copy_validated_entries(src_path, w, written);
  if(w)
  {
    if(archive_write_close(w) != ARCHIVE_OK) ok = FALSE;
    archive_write_free(w);
  }
  if(wf && fclose(wf) != 0) ok = FALSE;
  g_hash_table_unref(written);
  // g_mkstemp creates mode 0600. Keep the destination's permissions (or
  // the source's permissions for a new sidecar) across the atomic rename.
  GStatBuf mode_source = { 0 };
  const char *mode_path = g_file_test(dst_path, G_FILE_TEST_EXISTS) ? dst_path : src_path;
  if(ok && (g_stat(mode_path, &mode_source) != 0
            || g_chmod(tmp, mode_source.st_mode & 0777) != 0)) ok = FALSE;
  if(ok && g_rename(tmp, dst_path) != 0) ok = FALSE;
  if(!ok) g_unlink(tmp);
  g_free(tmp);
  g_rw_lock_writer_unlock(&_lock);
  return ok;
}

gboolean dt_dtdata_file_sweep(const char *path,
                              const GList *keep,
                              const uint32_t kinds)
{
  if(!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return TRUE;
  // listed and rewritten under one lock, so a concurrent write is either
  // in the listing or after the rewrite, never lost in between
  g_rw_lock_writer_lock(&_lock);
  // a zip that cannot be read through would look emptier than it is
  gboolean listed = FALSE;
  GList *entries = _zip_list_unlocked(path, &listed);
  if(!listed || !entries)
  {
    g_rw_lock_writer_unlock(&_lock);
    return listed;
  }

  GList *drop = NULL;
  int remaining = 0;
  for(GList *l = entries; l; l = g_list_next(l))
  {
    const char *name = l->data;
    const int kind = dt_dtdata_entry_kind(name);
    const gboolean claimed = kind >= 0 && (kinds & (1u << kind));
    if(claimed && !g_list_find_custom((GList *)keep, name, (GCompareFunc)g_strcmp0))
      drop = g_list_prepend(drop, g_strdup(name));
    else
      remaining++;
  }
  g_list_free_full(entries, g_free);

  gboolean ok = TRUE;
  if(drop)
  {
    if(remaining == 0)
      ok = g_unlink(path) == 0;
    else
      ok = _zip_rewrite(path, drop, NULL, NULL, 0);
    dt_print(DT_DEBUG_MASKS, "[dtdata] swept %d unreferenced entries from '%s'%s",
             g_list_length(drop), path, remaining ? "" : ", file removed");
  }
  g_rw_lock_writer_unlock(&_lock);
  g_list_free_full(drop, g_free);
  return ok;
}

// --- by image ---

void dt_dtdata_register_scanner(const char *op,
                                const uint32_t kinds,
                                dt_dtdata_scan_fn fn)
{
  if(!op || !fn) return;
  _scanner_t *s = g_new0(_scanner_t, 1);
  g_strlcpy(s->op, op, sizeof(s->op));
  s->kinds = kinds;
  s->fn = fn;
  _scanners = g_list_append(_scanners, s);
}

static const _scanner_t *_scanner_for(const char *op)
{
  for(const GList *l = _scanners; l; l = g_list_next(l))
    if(!strcmp(((_scanner_t *)l->data)->op, op)) return l->data;
  return NULL;
}

void dt_dtdata_sweep(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid) || !_scanners) return;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0] || !g_file_test(path, G_FILE_TEST_EXISTS)) return;

  uint32_t kinds = 0;
  for(const GList *l = _scanners; l; l = g_list_next(l))
    kinds |= ((_scanner_t *)l->data)->kinds;

  GList *keep = NULL;
  gboolean readable = TRUE;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT operation, op_params, module"
                              " FROM main.history"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(readable && sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op = (const char *)sqlite3_column_text(stmt, 0);
    const _scanner_t *s = op ? _scanner_for(op) : NULL;
    if(!s) continue;
    char entry[DT_DTDATA_ENTRY_LEN] = { 0 };
    const int res = s->fn(sqlite3_column_blob(stmt, 1),
                          sqlite3_column_bytes(stmt, 1),
                          sqlite3_column_int(stmt, 2),
                          entry, sizeof(entry));
    if(res > 0)
      keep = g_list_prepend(keep, g_strdup(entry));
    else if(res < 0)
      readable = FALSE;
  }
  sqlite3_finalize(stmt);

  // a params version this build cannot read might reference anything:
  // leave the file to the build that wrote it
  if(readable)
    dt_dtdata_file_sweep(path, keep, kinds);
  else
    dt_print(DT_DEBUG_MASKS, "[dtdata] history of image %d has unreadable params, '%s' not swept",
             imgid, path);
  g_list_free_full(keep, g_free);
}

gboolean dt_dtdata_write_gray(const dt_imgid_t imgid,
                              const dt_dtdata_kind_t kind,
                              const dt_dtdata_origin_t origin,
                              const char *producer,
                              const float *mask,
                              const int width,
                              const int height,
                              const int bpc,
                              dt_dtdata_ref_t *ref)
{
  if(!dt_dtdata_enabled() || !dt_is_valid_imgid(imgid)) return FALSE;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return FALSE;
  return dt_dtdata_file_write_gray(path, kind, origin, producer, mask, width, height, bpc, ref);
}

gboolean dt_dtdata_write_rgba(const dt_imgid_t imgid,
                              const char *producer,
                              const float *rgba,
                              const int width,
                              const int height,
                              const int bpc,
                              dt_dtdata_ref_t *ref)
{
  if(!dt_dtdata_enabled() || !dt_is_valid_imgid(imgid)) return FALSE;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return FALSE;
  return dt_dtdata_file_write_rgba(path, producer, rgba, width, height, bpc, ref);
}

// the sidecar beside the local copy, empty when the image has none. a mask
// imported while the original was offline lives there until
// dt_image_local_copy_reset() merges it beside the original
static void _cache_path(const dt_imgid_t imgid, char *path, const size_t len)
{
  path[0] = 0;
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  const gboolean local_copy = img && (img->flags & DT_IMAGE_LOCAL_COPY);
  dt_image_cache_read_release(img);
  if(!local_copy) return;
  char image[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, image, sizeof(image), &from_cache);
  if(!from_cache || !image[0]) return;
  dt_image_path_append_version(imgid, image, sizeof(image));
  dt_dtdata_path_for_image(image, path, len);
}

float *dt_dtdata_read_gray(const dt_imgid_t imgid,
                           const dt_dtdata_ref_t *ref,
                           int *width,
                           int *height)
{
  *width = *height = 0;
  if(!dt_is_valid_imgid(imgid)) return NULL;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return NULL;
  float *mask = dt_dtdata_file_read_gray(path, ref, width, height);
  if(mask) return mask;

  char cache[PATH_MAX] = { 0 };
  _cache_path(imgid, cache, sizeof(cache));
  if(!cache[0] || !strcmp(cache, path) || !g_file_test(cache, G_FILE_TEST_EXISTS))
    return NULL;
  dt_print(DT_DEBUG_MASKS, "[dtdata] entry '%s' not beside the original, trying '%s'",
           ref ? ref->entry : "", cache);
  return dt_dtdata_file_read_gray(cache, ref, width, height);
}

float *dt_dtdata_read_rgba(const dt_imgid_t imgid,
                           const dt_dtdata_ref_t *ref,
                           int *width,
                           int *height)
{
  *width = *height = 0;
  if(!dt_is_valid_imgid(imgid)) return NULL;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return NULL;
  float *rgba = dt_dtdata_file_read_rgba(path, ref, width, height);
  if(rgba) return rgba;

  char cache[PATH_MAX] = { 0 };
  _cache_path(imgid, cache, sizeof(cache));
  if(!cache[0] || !strcmp(cache, path) || !g_file_test(cache, G_FILE_TEST_EXISTS))
    return NULL;
  dt_print(DT_DEBUG_MASKS, "[dtdata] entry '%s' not beside the original, trying '%s'",
           ref ? ref->entry : "", cache);
  return dt_dtdata_file_read_rgba(cache, ref, width, height);
}

GList *dt_dtdata_list_entries(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return NULL;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return NULL;
  return dt_dtdata_file_list_entries(path);
}

static gboolean _source_copyable(const dt_imgid_t imgid,
                                 const char *src,
                                 const char *cache,
                                 const gboolean validate_archives,
                                 GList **needed_out)
{
  GList *needed = NULL;
  gboolean ok = TRUE;
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  const int prepared = sqlite3_prepare_v2(dt_database_get(darktable.db),
                              "SELECT operation, op_params, module"
                              " FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  if(prepared != SQLITE_OK) ok = FALSE;
  if(ok) DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  int rc = SQLITE_DONE;
  while(ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
  {
    const char *op = (const char *)sqlite3_column_text(stmt, 0);
    const _scanner_t *s = op ? _scanner_for(op) : NULL;
    if(!s) continue;
    char entry[DT_DTDATA_ENTRY_LEN] = { 0 };
    const int result = s->fn(sqlite3_column_blob(stmt, 1),
                             sqlite3_column_bytes(stmt, 1),
                             sqlite3_column_int(stmt, 2),
                             entry, sizeof(entry));
    if(result < 0) ok = FALSE;
    else if(result > 0) needed = g_list_prepend(needed, g_strdup(entry));
  }
  if(ok && rc != SQLITE_DONE) ok = FALSE;
  if(stmt) sqlite3_finalize(stmt);

  const gboolean cache_exists = cache[0] && strcmp(cache, src)
                                && g_file_test(cache, G_FILE_TEST_EXISTS);
  for(GList *l = needed; l && ok; l = g_list_next(l))
  {
    const char *entry = l->data;
    GBytes *bytes = g_file_test(src, G_FILE_TEST_EXISTS) ? _zip_read(src, entry) : NULL;
    if(!bytes && cache_exists) bytes = _zip_read(cache, entry);
    if(!bytes)
      ok = FALSE;
    else
    {
      gsize len = 0;
      const void *data = g_bytes_get_data(bytes, &len);
      ok = _entry_matches_bytes(entry, data, len);
      g_bytes_unref(bytes);
    }
  }
  if(ok && validate_archives)
  {
    // Match the whole-file checks performed by the later merge. An
    // unreferenced damaged entry would make that merge fail too.
    g_rw_lock_reader_lock(&_lock);
    if(g_file_test(src, G_FILE_TEST_EXISTS))
      ok = _zip_copy_validated_entries(src, NULL, NULL);
    if(ok && cache_exists)
      ok = _zip_copy_validated_entries(cache, NULL, NULL);
    g_rw_lock_reader_unlock(&_lock);
  }
  if(ok && needed_out)
    *needed_out = needed;
  else
    g_list_free_full(needed, g_free);
  return ok;
}

static gboolean _sidecar_path_absent(const char *path)
{
  if(!path[0]) return TRUE;
  GStatBuf st;
  if(g_lstat(path, &st) == 0) return FALSE;
  // a broken link or an unreadable path must not turn into a safe copy
  return errno == ENOENT;
}

gboolean dt_dtdata_history_copyable(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return FALSE;

  // Darkroom edits can still be in memory. Check the same stored history
  // that the subsequent copy-and-paste operation will use.
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_dev_write_history(darktable.develop);

  char src[PATH_MAX] = { 0 }, cache[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, src, sizeof(src));
  if(!src[0]) return FALSE;
  _cache_path(imgid, cache, sizeof(cache));
  // a later merge copies the whole archive, even unreferenced entries
  // refuse before creating a row or photo file until sidecar copies can
  // roll back transactionally
  if(!_sidecar_path_absent(src)
     || (cache[0] && strcmp(cache, src) && !_sidecar_path_absent(cache)))
    return FALSE;
  return _source_copyable(imgid, src, cache, TRUE, NULL);
}

gboolean dt_dtdata_merge(const dt_imgid_t src_imgid, const dt_imgid_t dst_imgid)
{
  if(!dt_is_valid_imgid(src_imgid) || !dt_is_valid_imgid(dst_imgid)) return FALSE;
  char src[PATH_MAX] = { 0 }, dst[PATH_MAX] = { 0 }, cache[PATH_MAX] = { 0 };
  dt_dtdata_path(src_imgid, src, sizeof(src));
  dt_dtdata_path(dst_imgid, dst, sizeof(dst));
  if(!src[0] || !dst[0]) return FALSE;
  _cache_path(src_imgid, cache, sizeof(cache));

  // Check every known reference before changing destination history. A
  // missing sidecar is harmless only if no copied row needs it.
  GList *needed = NULL;
  if(!_source_copyable(src_imgid, src, cache, FALSE, &needed)) return FALSE;
  gboolean ok = dt_dtdata_file_merge(src, dst);
  const gboolean cache_exists = cache[0] && strcmp(cache, src)
                                && g_file_test(cache, G_FILE_TEST_EXISTS);
  if(ok && cache_exists) ok = dt_dtdata_file_merge(cache, dst);
  for(GList *l = needed; l && ok; l = g_list_next(l))
  {
    GBytes *bytes = _zip_read(dst, l->data);
    if(!bytes)
      ok = FALSE;
    else
    {
      gsize len = 0;
      const void *data = g_bytes_get_data(bytes, &len);
      ok = _entry_matches_bytes(l->data, data, len);
      g_bytes_unref(bytes);
    }
  }
  g_list_free_full(needed, g_free);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
