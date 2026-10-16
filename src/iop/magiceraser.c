/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/dtdata.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/iop_profile.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "iop/magiceraser.h"
#include "common/ai/inpaint.h"
#include "iop/magiceraser_preview.h"

#include <math.h>
#include <inttypes.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* One accepted fill per instance. A further erasure gets a further history
   instance, so each accepted result can be undone independently. Stored patch
   RGB is canonical, encoded sRGB; each pipe converts it to its output profile
   before blending. Alpha is coverage and is not color-managed. */
DT_MODULE_INTROSPECTION(3, dt_iop_magiceraser_params_t)

typedef struct dt_iop_magiceraser_params_v1_t
{
  char entry[DT_DTDATA_ENTRY_LEN];
  int entry_kind;
  int entry_origin;
  int entry_width;
  int entry_height;
  int entry_bpc;
  char entry_producer[DT_DTDATA_PRODUCER_LEN];
  int patch_x;
  int patch_y;
  int canvas_width;
  int canvas_height;
} dt_iop_magiceraser_params_v1_t;

typedef struct dt_iop_magiceraser_params_v2_t
{
  char entry[DT_DTDATA_ENTRY_LEN];
  int entry_kind;
  int entry_origin;
  int entry_width;
  int entry_height;
  int entry_bpc;
  char entry_producer[DT_DTDATA_PRODUCER_LEN];
  int patch_x;
  int patch_y;
  int canvas_width;
  int canvas_height;
  dt_hash_t geometry_hash;
} dt_iop_magiceraser_params_v2_t;

typedef enum dt_iop_magiceraser_patch_status_t
{
  DT_MAGICERASER_PATCH_NONE,
  DT_MAGICERASER_PATCH_READY,
  DT_MAGICERASER_PATCH_INVALID_REF,
  DT_MAGICERASER_PATCH_GEOMETRY_CHANGED,
  DT_MAGICERASER_PATCH_SOURCE_CHANGED,
  DT_MAGICERASER_PATCH_MISSING
} dt_iop_magiceraser_patch_status_t;

typedef struct dt_magiceraser_file_identity_t
{
  gboolean present;
  struct stat stat;
} dt_magiceraser_file_identity_t;

typedef struct dt_magiceraser_sidecar_snapshot_t
{
  char path[2][PATH_MAX]; // preferred sidecar, then the local-copy fallback
  dt_magiceraser_file_identity_t file[2];
} dt_magiceraser_sidecar_snapshot_t;

typedef struct dt_iop_magiceraser_data_t
{
  /* Tiled process calls may share a pipe piece. Profile conversion replaces
     rgba, so keep its preparation and sampling in one critical section. */
  GMutex patch_lock;
  float *rgba_srgb;
  float *rgba;
  const dt_iop_order_iccprofile_info_t *srgb_profile;
  const dt_iop_order_iccprofile_info_t *output_profile;
  int width;
  int height;
  int patch_x;
  int patch_y;
  int canvas_width;
  int canvas_height;
  dt_hash_t geometry_hash;
  gboolean geometry_valid;
  gboolean legacy_geometry;
  gboolean runtime_geometry_invalid;
  gboolean runtime_source_invalid;
  gboolean runtime_sidecar_invalid;
  guint64 sidecar_revision;
  char source_path[PATH_MAX];
  struct stat source_stat;
  dt_magiceraser_sidecar_snapshot_t sidecar;
  int sidecar_selected;
  dt_imgid_t status_imgid;
  int32_t status_history_end;
  dt_iop_magiceraser_params_t status_params;
  dt_iop_magiceraser_patch_status_t patch_status;
} dt_iop_magiceraser_data_t;

typedef struct dt_iop_magiceraser_gui_data_t
{
  GtkWidget *hint;
  GtkWidget *paint;
  dt_imgid_t status_imgid;
  int32_t status_history_end;
  dt_iop_magiceraser_params_t status_params;
  dt_iop_magiceraser_patch_status_t patch_status;
  gboolean status_valid;
  gboolean patch_status_shown;
  GtkWidget *generate;
  struct dt_magiceraser_preview_state_t *preview_state;
} dt_iop_magiceraser_gui_data_t;

static gboolean _source_stat_same(const struct stat *a, const struct stat *b)
{
  gboolean same = S_ISREG(a->st_mode) && S_ISREG(b->st_mode)
                  && a->st_dev == b->st_dev && a->st_ino == b->st_ino
                  && a->st_size == b->st_size
                  && a->st_mtime == b->st_mtime
                  && a->st_ctime == b->st_ctime;
#if defined(__APPLE__)
  same = same && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec
              && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#elif defined(__linux__)
  same = same && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec
              && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
  return same;
}

/* Keep this fallback resolution in step with dtdata.c's private _cache_path().
   It is deliberately private here: introducing a new public dtdata API would
   require a separate approval. Both candidates are tracked, including an
   absent preferred sidecar that could later supersede a local-copy entry. */
static gboolean _sidecar_snapshot(const dt_imgid_t imgid,
                                  dt_magiceraser_sidecar_snapshot_t *snapshot)
{
  memset(snapshot, 0, sizeof(*snapshot));
  dt_dtdata_path(imgid, snapshot->path[0], sizeof(snapshot->path[0]));
  if(!snapshot->path[0]) return FALSE;

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  const gboolean local_copy = img && (img->flags & DT_IMAGE_LOCAL_COPY);
  dt_image_cache_read_release(img);
  if(local_copy)
  {
    char image[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, image, sizeof(image), &from_cache);
    if(from_cache && image[0])
    {
      dt_image_path_append_version(imgid, image, sizeof(image));
      dt_dtdata_path_for_image(image, snapshot->path[1], sizeof(snapshot->path[1]));
      if(!strcmp(snapshot->path[0], snapshot->path[1])) snapshot->path[1][0] = '\0';
    }
  }

  for(int i = 0; i < 2; i++)
  {
    if(!snapshot->path[i][0]) continue;
    if(g_stat(snapshot->path[i], &snapshot->file[i].stat) == 0)
    {
      if(!S_ISREG(snapshot->file[i].stat.st_mode)) return FALSE;
      snapshot->file[i].present = TRUE;
    }
    else if(errno != ENOENT) return FALSE;
  }
  return TRUE;
}

static gboolean _sidecar_snapshot_same(const dt_magiceraser_sidecar_snapshot_t *a,
                                       const dt_magiceraser_sidecar_snapshot_t *b)
{
  for(int i = 0; i < 2; i++)
    if(strcmp(a->path[i], b->path[i])
       || a->file[i].present != b->file[i].present
       || (a->file[i].present
           && !_source_stat_same(&a->file[i].stat, &b->file[i].stat))) return FALSE;
  return TRUE;
}

/* A process worker can see a changed source or sidecar before the GUI watcher
   has acquired its first baseline. Queue one cache refresh on the GUI thread;
   only a stable, content-checked recommit may install a new patch baseline. */
G_LOCK_DEFINE_STATIC(magiceraser_sidecar_refresh);
static GHashTable *_sidecar_refresh_pending;

static void _queue_sidecar_refresh(const dt_imgid_t imgid)
{
  G_LOCK(magiceraser_sidecar_refresh);
  if(!_sidecar_refresh_pending)
    _sidecar_refresh_pending = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_add(_sidecar_refresh_pending, GINT_TO_POINTER(imgid));
  G_UNLOCK(magiceraser_sidecar_refresh);
}

static gboolean _take_sidecar_refresh(const dt_imgid_t imgid)
{
  G_LOCK(magiceraser_sidecar_refresh);
  const gboolean pending = _sidecar_refresh_pending
    && g_hash_table_remove(_sidecar_refresh_pending, GINT_TO_POINTER(imgid));
  G_UNLOCK(magiceraser_sidecar_refresh);
  return pending;
}

/* The pixelpipe may reuse an already composited tile without calling process().
   Watch the active source from the GUI thread so an external replacement also
   invalidates those cached tiles. This is deliberately a single watcher for
   all instances and does not attempt to evict a RAW buffer held by a worker. */
static struct
{
  guint timer;
  guint users;
  dt_imgid_t imgid;
  gboolean have_baseline;
  gboolean invalidated;
  gboolean waiting_for_recommit;
  char path[PATH_MAX];
  struct stat source_stat;
  dt_magiceraser_sidecar_snapshot_t sidecar;
} _source_watch;

static gboolean _source_watch_tick(gpointer user_data)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || dev->gui_leaving
     || !dt_is_valid_imgid(dev->image_storage.id)) return G_SOURCE_CONTINUE;

  dt_iop_module_t *first = NULL;
  for(GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *module = iter->data;
    const dt_iop_magiceraser_params_t *p = module->params;
    if(!strcmp(module->op, "magiceraser") && module->enabled && p && p->entry[0]
       && (!first || module->iop_order < first->iop_order)) first = module;
  }
  if(!first)
  {
    _source_watch.have_baseline = FALSE;
    _source_watch.invalidated = FALSE;
    _source_watch.waiting_for_recommit = FALSE;
    return G_SOURCE_CONTINUE;
  }

  const dt_imgid_t imgid = dev->image_storage.id;
  if(_source_watch.imgid != imgid)
  {
    _take_sidecar_refresh(_source_watch.imgid);
    _source_watch.imgid = imgid;
    _source_watch.have_baseline = FALSE;
    _source_watch.invalidated = FALSE;
    _source_watch.waiting_for_recommit = FALSE;
  }
  if(_take_sidecar_refresh(imgid))
  {
    _source_watch.have_baseline = FALSE;
    _source_watch.waiting_for_recommit = TRUE;
    dt_iop_refresh_all(first);
    return G_SOURCE_CONTINUE;
  }

  dt_magiceraser_sidecar_snapshot_t sidecar_now;
  const gboolean sidecar_available = _sidecar_snapshot(imgid, &sidecar_now);
  char path[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, path, sizeof(path), &from_cache);
  struct stat now = { 0 };
  const gboolean source_available = path[0] && g_stat(path, &now) == 0;
  if(!_source_watch.have_baseline)
  {
    dt_dev_pixelpipe_t *pipe = dev->full.pipe;
    if(!pipe) return G_SOURCE_CONTINUE;
    dt_pthread_mutex_lock(&pipe->mutex);
    for(GList *iter = dev->iop; iter && !_source_watch.have_baseline; iter = g_list_next(iter))
    {
      dt_iop_module_t *module = iter->data;
      const dt_iop_magiceraser_params_t *p = module->params;
      if(strcmp(module->op, "magiceraser") || !module->enabled || !p || !p->entry[0]) continue;
      dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(dev, pipe, module);
      if(!piece || !piece->enabled || !piece->data) continue;
      dt_iop_magiceraser_data_t *d = piece->data;
      g_mutex_lock(&d->patch_lock);
      if(d->patch_status == DT_MAGICERASER_PATCH_READY && d->status_imgid == imgid
         && !d->runtime_source_invalid && !d->runtime_sidecar_invalid
         && d->source_path[0]
         && (!_source_watch.invalidated
             || (source_available && !strcmp(path, d->source_path)
                 && _source_stat_same(&now, &d->source_stat)))
         && (!_source_watch.waiting_for_recommit
             || (sidecar_available
                 && _sidecar_snapshot_same(&d->sidecar, &sidecar_now))))
      {
        g_strlcpy(_source_watch.path, d->source_path, sizeof(_source_watch.path));
        _source_watch.source_stat = d->source_stat;
        _source_watch.sidecar = d->sidecar;
        _source_watch.have_baseline = TRUE;
        _source_watch.invalidated = FALSE;
        _source_watch.waiting_for_recommit = FALSE;
      }
      g_mutex_unlock(&d->patch_lock);
    }
    dt_pthread_mutex_unlock(&pipe->mutex);
    if(!_source_watch.have_baseline) return G_SOURCE_CONTINUE;
  }

  const gboolean source_same = source_available && !strcmp(path, _source_watch.path)
                               && _source_stat_same(&_source_watch.source_stat, &now);
  const gboolean sidecar_same = sidecar_available
    && _sidecar_snapshot_same(&_source_watch.sidecar, &sidecar_now);
  if(source_same && sidecar_same) return G_SOURCE_CONTINUE;

  _source_watch.invalidated = !source_same;
  _source_watch.have_baseline = FALSE;
  if(!sidecar_same)
  {
    _source_watch.waiting_for_recommit = TRUE;
  }
  _take_sidecar_refresh(imgid);
  /* Hide the old patch before invalidating cached tiles. A fresh pipe sync
     may restore it only through a stable, content-checked RGBA read. */
  dt_dev_pixelpipe_t *pipe = dev->full.pipe;
  if(pipe)
  {
    dt_pthread_mutex_lock(&pipe->mutex);
    for(GList *iter = dev->iop; iter; iter = g_list_next(iter))
    {
      dt_iop_module_t *module = iter->data;
      if(strcmp(module->op, "magiceraser")) continue;
      dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(dev, pipe, module);
      if(!piece || !piece->data) continue;
      dt_iop_magiceraser_data_t *d = piece->data;
      g_mutex_lock(&d->patch_lock);
      if(d->status_imgid == imgid && d->status_params.entry[0])
      {
        d->runtime_source_invalid |= !source_same;
        d->runtime_sidecar_invalid |= !sidecar_same;
      }
      g_mutex_unlock(&d->patch_lock);
    }
    dt_pthread_mutex_unlock(&pipe->mutex);
  }
  /* Re-render from the earliest accepted patch. The per-piece stat guards
     hide changed input; a source RAW cache may still need an app restart. */
  dt_iop_refresh_all(first);
  return G_SOURCE_CONTINUE;
}

/* Worker and idle callback can outlive gui_cleanup or a darkroom image
   switch. Only the GTK thread reads owner and generation. */
typedef struct dt_magiceraser_preview_state_t
{
  gint refs;
  guint generation;
  gboolean running;
  dt_iop_module_t *owner;
} dt_magiceraser_preview_state_t;

typedef struct dt_magiceraser_job_params_t
{
  dt_magiceraser_preview_state_t *state;
  guint generation;
  dt_imgid_t imgid;
  int multi_priority;
  int32_t history_end;
  dt_mask_id_t mask_id;
  dt_hash_t group_hash;
  dt_hash_t geometry_hash;
  GBytes *history_digest;
} dt_magiceraser_job_params_t;

typedef struct dt_magiceraser_job_result_t
{
  dt_magiceraser_preview_state_t *state;
  guint generation;
  GBytes *history_digest;
  dt_magiceraser_preview_t *preview;
  char *error;
} dt_magiceraser_job_result_t;

const char *name()
{
  return _("magic eraser");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
      _("apply an accepted, non-destructive inpainted color patch"),
      _("corrective"),
      _("linear, RGB, display-referred"),
      _("linear, RGB"),
      _("linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT;
}

int flags()
{
  /* A provisional, pixel-neutral instance owns the user's brush selection.
     The accepted color patch already has coverage, so its selection must not
     also become an output blend mask. */
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_NO_MASKS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static int _scan_params(const void *params,
                        const size_t size,
                        const int version,
                        char *entry,
                        const size_t len)
{
  if((version != 1 || size != sizeof(dt_iop_magiceraser_params_v1_t))
     && (version != 2 || size != sizeof(dt_iop_magiceraser_params_v2_t))
     && (version != 3 || size != sizeof(dt_iop_magiceraser_params_t)))
    return -1;
  const dt_iop_magiceraser_params_v1_t *p = params;
  if(!memchr(p->entry, '\0', sizeof(p->entry))) return -1;
  if(!p->entry[0]) return 0;
  g_strlcpy(entry, p->entry, len);
  return 1;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if((old_version != 1 && old_version != 2) || !old_params) return 1;
  dt_iop_magiceraser_params_t *p = calloc(1, sizeof(*p));
  if(!p) return 1;
  memcpy(p, old_params, old_version == 1
           ? sizeof(dt_iop_magiceraser_params_v1_t)
           : sizeof(dt_iop_magiceraser_params_v2_t));
  /* v1 cannot prove alignment after a crop or rotation, so it fails closed. */
  if(old_version == 1) p->geometry_hash = DT_INVALID_HASH;
  /* Neither prototype version attested the source photo bytes. Keep the
     sidecar reference for recovery, but never apply it to an unknown photo. */
  *new_params = p;
  *new_params_size = sizeof(*p);
  *new_version = 3;
  return 0;
}

void init_global(dt_iop_module_so_t *self)
{
  dt_dtdata_register_scanner(self->op, 1u << DT_DTDATA_KIND_PATCH, _scan_params);
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
  self->default_enabled = FALSE;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_magiceraser_data_t *d = calloc(1, sizeof(*d));
  if(d) g_mutex_init(&d->patch_lock);
  piece->data = d;
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_magiceraser_data_t *d = piece->data;
  if(d)
  {
    g_mutex_lock(&d->patch_lock);
    dt_free_align(d->rgba_srgb);
    dt_free_align(d->rgba);
    g_mutex_unlock(&d->patch_lock);
    g_mutex_clear(&d->patch_lock);
    free(d);
  }
  piece->data = NULL;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_magiceraser_params_t *p = (const dt_iop_magiceraser_params_t *)params;
  dt_iop_magiceraser_data_t *d = piece->data;
  if(!d) return;
  const dt_hash_t current_geometry = dt_magiceraser_geometry_hash(self->dev,
                                                                   self->iop_order);
  const dt_hash_t legacy_geometry = dt_magiceraser_geometry_hash_legacy(self->dev,
                                                                         self->iop_order);
  dt_magiceraser_sidecar_snapshot_t sidecar_now;
  const gboolean sidecar_available = _sidecar_snapshot(pipe->image.id, &sidecar_now);
  g_mutex_lock(&d->patch_lock);
  /* A sync first commits defaults and only then replays accepted history.
     Retain the last verified file identity through that defaults pass, so a
     replacement in the interval cannot become the new baseline. */
  const gboolean have_previous_sidecar = d->status_imgid == pipe->image.id
    && d->sidecar.path[0][0];
  const gboolean was_runtime_sidecar_invalid = d->status_imgid == pipe->image.id
    && d->runtime_sidecar_invalid;
  const dt_magiceraser_sidecar_snapshot_t previous_sidecar = d->sidecar;
  gboolean previous_sidecar_changed = have_previous_sidecar
    && (!sidecar_available || !_sidecar_snapshot_same(&previous_sidecar, &sidecar_now));
  const gboolean runtime_sidecar_invalid = previous_sidecar_changed
    || was_runtime_sidecar_invalid;
  d->sidecar_revision++;
  dt_free_align(d->rgba_srgb);
  d->rgba_srgb = NULL;
  dt_free_align(d->rgba);
  d->rgba = NULL;
  d->srgb_profile = NULL;
  d->output_profile = NULL;
  d->width = d->height = 0;
  d->patch_x = p->patch_x;
  d->patch_y = p->patch_y;
  d->canvas_width = p->canvas_width;
  d->canvas_height = p->canvas_height;
  d->geometry_hash = p->geometry_hash;
  d->status_imgid = pipe->image.id;
  d->status_history_end = self->dev->history_end;
  memcpy(&d->status_params, p, sizeof(*p));
  d->patch_status = p->entry[0] ? DT_MAGICERASER_PATCH_INVALID_REF
                                : DT_MAGICERASER_PATCH_NONE;
  d->runtime_geometry_invalid = FALSE;
  d->runtime_source_invalid = FALSE;
  d->runtime_sidecar_invalid = runtime_sidecar_invalid;
  d->source_path[0] = '\0';
  memset(&d->source_stat, 0, sizeof(d->source_stat));
  if(!have_previous_sidecar) memset(&d->sidecar, 0, sizeof(d->sidecar));
  d->sidecar_selected = -1;
  d->legacy_geometry = p->geometry_hash != current_geometry
                       && p->geometry_hash == legacy_geometry;
  d->geometry_valid = p->geometry_hash != DT_INVALID_HASH
                      && (p->geometry_hash == current_geometry
                          || d->legacy_geometry);
  if(p->entry[0] && !d->geometry_valid)
    dt_print(DT_DEBUG_PARAMS,
             "[magic eraser] patch geometry differs: saved=%" PRIu64 " current=%" PRIu64,
             p->geometry_hash, current_geometry);
  if(!p->entry[0]) goto unlock;
  if(!memchr(p->entry, '\0', sizeof(p->entry))
     || !memchr(p->entry_producer, '\0', sizeof(p->entry_producer))
     || p->entry_kind != DT_DTDATA_KIND_PATCH
     || p->entry_origin != DT_DTDATA_ORIGIN_AUTHORITATIVE
     || p->patch_x < 0 || p->patch_y < 0
     || p->canvas_width <= 0 || p->canvas_height <= 0
     || p->entry_width <= 0 || p->entry_height <= 0
     || p->patch_x > p->canvas_width - p->entry_width
     || p->patch_y > p->canvas_height - p->entry_height)
    goto unlock;
  if(!d->geometry_valid)
  {
    d->patch_status = DT_MAGICERASER_PATCH_GEOMETRY_CHANGED;
    goto unlock;
  }

  // The source-byte guard is expensive for a RAW file. Run it only for a
  // structurally valid accepted patch, never for defaults or stale geometry.
  char source_path[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(pipe->image.id, source_path, sizeof(source_path), &from_cache);
  struct stat source_before = { 0 }, source_after = { 0 };
  char current_sha256[DT_DTDATA_SOURCE_SHA256_LEN] = { 0 };
  if(!dt_magiceraser_valid_sha256(p->source_sha256)
     || !source_path[0]
     || g_stat(source_path, &source_before) != 0
     || !dt_dtdata_file_source_sha256(source_path, current_sha256)
     || g_stat(source_path, &source_after) != 0
     || !_source_stat_same(&source_before, &source_after)
     || strcmp(p->source_sha256, current_sha256))
  {
    d->patch_status = DT_MAGICERASER_PATCH_SOURCE_CHANGED;
    dt_print(DT_DEBUG_ALWAYS,
             "[magic eraser] source photo differs or is unavailable; accepted patch is not applied");
    goto unlock;
  }

  dt_dtdata_ref_t ref = { 0 };
  g_strlcpy(ref.entry, p->entry, sizeof(ref.entry));
  ref.kind = p->entry_kind;
  ref.origin = p->entry_origin;
  ref.width = p->entry_width;
  ref.height = p->entry_height;
  ref.bpc = p->entry_bpc;
  g_strlcpy(ref.producer, p->entry_producer, sizeof(ref.producer));
  dt_magiceraser_sidecar_snapshot_t sidecar_before, sidecar_after;
  if(_sidecar_snapshot(pipe->image.id, &sidecar_before))
  {
    if(have_previous_sidecar
       && !_sidecar_snapshot_same(&previous_sidecar, &sidecar_before))
      previous_sidecar_changed = TRUE;
    /* Match dt_dtdata_read_rgba()'s preferred-then-local-copy order while
       retaining the exact file used. Both candidates must remain unchanged
       across the checked ZIP/PNG read, including an absent preferred file. */
    for(int i = 0; i < 2 && !d->rgba_srgb; i++)
      if(sidecar_before.path[i][0] && sidecar_before.file[i].present)
      {
        d->rgba_srgb = dt_dtdata_file_read_rgba(sidecar_before.path[i], &ref,
                                                &d->width, &d->height);
        if(d->rgba_srgb) d->sidecar_selected = i;
      }
    const gboolean stable_read = _sidecar_snapshot(pipe->image.id, &sidecar_after)
      && _sidecar_snapshot_same(&sidecar_before, &sidecar_after);
    if(!stable_read)
    {
      if(have_previous_sidecar) previous_sidecar_changed = TRUE;
      dt_free_align(d->rgba_srgb);
      d->rgba_srgb = NULL;
      d->width = d->height = 0;
      d->sidecar_selected = -1;
    }
  }
  else if(have_previous_sidecar) previous_sidecar_changed = TRUE;
  if(!d->rgba_srgb)
  {
    if(have_previous_sidecar) previous_sidecar_changed = TRUE;
    d->patch_status = DT_MAGICERASER_PATCH_MISSING;
    dt_print(DT_DEBUG_ALWAYS, "[magic eraser] missing or invalid patch '%s'", ref.entry);
  }
  else
  {
    d->patch_status = DT_MAGICERASER_PATCH_READY;
    d->runtime_sidecar_invalid = FALSE;
    d->sidecar = sidecar_after;
    g_strlcpy(d->source_path, source_path, sizeof(d->source_path));
    d->source_stat = source_after;
    d->srgb_profile = dt_ioppr_add_profile_info_to_list(self->dev,
                           DT_COLORSPACE_SRGB, "", DT_INTENT_PERCEPTUAL);
  }
unlock:
  {
    const gboolean recovered_sidecar = was_runtime_sidecar_invalid
      && d->patch_status == DT_MAGICERASER_PATCH_READY;
    g_mutex_unlock(&d->patch_lock);
    if((previous_sidecar_changed && !was_runtime_sidecar_invalid) || recovered_sidecar)
      _queue_sidecar_refresh(pipe->image.id);
  }
}

static gboolean _prepare_output_patch(dt_iop_module_t *self,
                                      dt_dev_pixelpipe_iop_t *piece,
                                      dt_iop_magiceraser_data_t *d)
{
  const dt_iop_order_iccprofile_info_t *const output =
    dt_ioppr_get_pipe_output_profile_info(piece->pipe);
  if(!output || output->type == DT_COLORSPACE_NONE
     || output->type == DT_COLORSPACE_LAB)
    return FALSE;
  if(d->rgba && d->output_profile == output) return TRUE;

  dt_free_align(d->rgba);
  d->rgba = NULL;
  d->output_profile = NULL;
  if(!d->srgb_profile) return FALSE;
  d->rgba = dt_alloc_align_float(4 * (size_t)d->width * d->height);
  if(!d->rgba) return FALSE;
  memcpy(d->rgba, d->rgba_srgb, 4 * (size_t)d->width * d->height * sizeof(float));
  dt_ioppr_transform_image_colorspace_rgb(d->rgba, d->rgba,
                                           d->width, d->height,
                                           d->srgb_profile, output, self->op);
  d->output_profile = output;
  return TRUE;
}

/* Sample premultiplied patch color so transparent edge pixels cannot make a
   dark halo when the preview is scaled. Missing samples have zero alpha. */
static inline void _sample(const dt_iop_magiceraser_data_t *d,
                           const float x,
                           const float y,
                           float *const alpha,
                           float rgb[3])
{
  *alpha = 0.0f;
  rgb[0] = rgb[1] = rgb[2] = 0.0f;
  const int x0 = (int)floorf(x);
  const int y0 = (int)floorf(y);
  const float fx = x - x0;
  const float fy = y - y0;
  for(int j = 0; j < 2; j++)
    for(int i = 0; i < 2; i++)
    {
      const int sx = x0 + i;
      const int sy = y0 + j;
      if(sx < 0 || sy < 0 || sx >= d->width || sy >= d->height) continue;
      const float *const p = d->rgba + 4 * ((size_t)sy * d->width + sx);
      const float weight = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
      const float a = CLAMPF(p[3], 0.0f, 1.0f) * weight;
      *alpha += a;
      for(int c = 0; c < 3; c++) rgb[c] += p[c] * a;
    }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const float *const in = ivoid;
  float *const out = ovoid;
  if(roi_out->scale != roi_in->scale)
  {
    const dt_interpolation_t *it = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample(it, out, roi_out, in, roi_in);
  }
  else
    dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);

  dt_iop_magiceraser_data_t *d = piece->data;
  if(!d || roi_out->scale <= 0.0f
     || piece->buf_in.width <= 0 || piece->buf_in.height <= 0)
    return;
  /* A crop change can synchronize only the crop node, leaving this node's
     committed data intact. Check the active geometry on every render. */
  dt_pthread_mutex_lock(&self->dev->history_mutex);
  const dt_hash_t current_geometry = dt_magiceraser_geometry_hash(self->dev,
                                                                   self->iop_order);
  const dt_hash_t legacy_geometry = dt_magiceraser_geometry_hash_legacy(self->dev,
                                                                         self->iop_order);
  dt_pthread_mutex_unlock(&self->dev->history_mutex);
  dt_magiceraser_sidecar_snapshot_t sidecar_now;
  gboolean sidecar_available = FALSE;
  gboolean stable_revision = FALSE;
  /* Snapshot stat I/O outside the patch lock, but never compare that result
     against a newer recommit. A concurrent sync changes this revision. */
  for(int attempt = 0; attempt < 3; attempt++)
  {
    g_mutex_lock(&d->patch_lock);
    const guint64 revision = d->sidecar_revision;
    g_mutex_unlock(&d->patch_lock);
    sidecar_available = _sidecar_snapshot(piece->pipe->image.id, &sidecar_now);
    g_mutex_lock(&d->patch_lock);
    if(revision == d->sidecar_revision)
    {
      stable_revision = TRUE;
      break;
    }
    g_mutex_unlock(&d->patch_lock);
  }
  if(!stable_revision) return;
  const dt_hash_t expected_geometry = d->legacy_geometry
    ? legacy_geometry : current_geometry;
  if(!d->rgba_srgb || d->geometry_hash == DT_INVALID_HASH
     || d->geometry_hash != expected_geometry)
  {
    if(d->rgba_srgb) d->runtime_geometry_invalid = TRUE;
    goto unlock;
  }
  /* A source may be replaced while the pipe stays open. Do not apply a
     verified patch to newly processed pixels from a changed file. */
  struct stat source_now = { 0 };
  if(d->runtime_source_invalid || !d->source_path[0]
     || g_stat(d->source_path, &source_now) != 0
     || !_source_stat_same(&d->source_stat, &source_now))
  {
    const gboolean newly_invalid = !d->runtime_source_invalid;
    d->runtime_source_invalid = TRUE;
    if(newly_invalid) _queue_sidecar_refresh(piece->pipe->image.id);
    goto unlock;
  }
  if(d->runtime_sidecar_invalid || !sidecar_available || d->sidecar_selected < 0
     || !_sidecar_snapshot_same(&d->sidecar, &sidecar_now))
  {
    const gboolean newly_invalid = !d->runtime_sidecar_invalid;
    d->runtime_sidecar_invalid = TRUE;
    if(newly_invalid) _queue_sidecar_refresh(piece->pipe->image.id);
    goto unlock;
  }
  /* The output profile can change without re-committing module params.
     Prepare the pipe-local color copy at render time, after colorout has
     published the current profile; never convert the canonical patch. */
  if(!_prepare_output_patch(self, piece, d)) goto unlock;
  // a resize preserves the patch's position; upstream geometry changes do
  // not, even when a crop or half-turn preserves the canvas aspect ratio
  const float width_ratio = (float)d->canvas_width / piece->buf_in.width;
  const float height_ratio = (float)d->canvas_height / piece->buf_in.height;
  if(fabsf(width_ratio - height_ratio) > 0.002f * MAX(width_ratio, height_ratio))
  {
    d->runtime_geometry_invalid = TRUE;
    goto unlock;
  }
  d->runtime_geometry_invalid = FALSE;
  const float x_scale = width_ratio / roi_out->scale;
  const float y_scale = height_ratio / roi_out->scale;
  for(int y = 0; y < roi_out->height; y++)
    for(int x = 0; x < roi_out->width; x++)
    {
      const float px = (roi_out->x + x + 0.5f) * x_scale - d->patch_x - 0.5f;
      const float py = (roi_out->y + y + 0.5f) * y_scale - d->patch_y - 0.5f;
      if(px < -1.0f || py < -1.0f || px >= d->width || py >= d->height) continue;
      float a, rgb[3];
      _sample(d, px, py, &a, rgb);
      if(a <= 0.0f) continue;
      float *const dst = out + 4 * ((size_t)y * roi_out->width + x);
      for(int c = 0; c < 3; c++) dst[c] = dst[c] * (1.0f - a) + rgb[c];
    }
unlock:
  g_mutex_unlock(&d->patch_lock);
}

static void _preview_state_unref(dt_magiceraser_preview_state_t *state)
{
  if(state && g_atomic_int_dec_and_test(&state->refs)) g_free(state);
}

static GBytes *_history_digest(const dt_imgid_t imgid)
{
  dt_history_hash_values_t values = { 0 };
  dt_history_hash_read(imgid, &values);
  GBytes *result = values.current && values.current_len > 0
    ? g_bytes_new(values.current, values.current_len) : NULL;
  dt_history_hash_free(&values);
  return result;
}

static gboolean _preview_matches(dt_iop_module_t *self,
                                 const dt_magiceraser_preview_t *p,
                                 GBytes *digest)
{
  if(!self || !p || !p->result_validated || !self->dev || self->dev != darktable.develop
     || self->dev->image_storage.id != p->imgid
     || self->multi_priority != p->multi_priority
     || self->dev->history_end != p->history_end
     || !self->blend_params || self->blend_params->mask_id != p->mask_id
     || ((dt_iop_magiceraser_params_t *)self->params)->entry[0]
     || dt_magiceraser_geometry_hash(self->dev, self->iop_order) != p->geometry_hash)
    return FALSE;
  dt_masks_form_t *group = dt_masks_get_from_id(self->dev, p->mask_id);
  if(!group || dt_masks_group_hash_ext(DT_INITHASH, group,
                                       self->dev->forms) != p->group_hash)
    return FALSE;
  dt_dev_write_history(self->dev);
  GBytes *current = _history_digest(p->imgid);
  const gboolean same = current && digest && g_bytes_equal(current, digest);
  if(current) g_bytes_unref(current);
  return same;
}

static void _pixbuf_release(guchar *pixels, gpointer data)
{
  g_free(pixels);
}

static void _preview_rgb_pixel(const dt_magiceraser_preview_t *p,
                               const size_t src, const gboolean after,
                               guchar *dst)
{
  const float a = after ? CLAMPF(p->coverage[src], 0.0f, 1.0f) : 0.0f;
  for(int c = 0; c < 3; c++)
  {
    const float v = p->source_rgb[3 * src + c] * (1.0f - a)
                    + p->filled_rgb[3 * src + c] * a;
    dst[c] = (guchar)CLAMP((int)roundf(v * 255.0f), 0, 255);
  }
}

static GdkPixbuf *_preview_pixbuf(const dt_magiceraser_preview_t *p,
                                  const gboolean after)
{
  if(!p || p->width <= 0 || p->height <= 0 || !p->source_rgb
     || !p->filled_rgb || !p->coverage)
    return NULL;
  const float scale = fminf(1.0f, 520.0f / MAX(p->width, p->height));
  const int w = MAX(1, (int)roundf(p->width * scale));
  const int h = MAX(1, (int)roundf(p->height * scale));
  guchar *pixels = g_try_malloc((size_t)w * h * 3);
  if(!pixels) return NULL;
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const int sx = MIN(p->width - 1, (int)(x / scale));
      const int sy = MIN(p->height - 1, (int)(y / scale));
      const size_t src = (size_t)sy * p->width + sx;
      _preview_rgb_pixel(p, src, after, pixels + 3 * ((size_t)y * w + x));
    }
  GdkPixbuf *preview = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB,
                                                 FALSE, 8, w, h, 3 * w,
                                                 _pixbuf_release, NULL);
  if(!preview) g_free(pixels);
  return preview;
}

typedef struct _preview_detail_t
{
  const dt_magiceraser_preview_t *preview;
  GtkWidget *before;
  GtkWidget *after;
  GtkWidget *position;
  int x;
  int y;
  int width;
  int height;
} _preview_detail_t;

static GdkPixbuf *_detail_pixbuf(const _preview_detail_t *detail,
                                 const gboolean after)
{
  const int w = detail->width, h = detail->height;
  guchar *pixels = g_try_malloc((size_t)w * h * 3);
  if(!pixels) return NULL;
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const size_t src = (size_t)(detail->y + y) * detail->preview->width
                         + detail->x + x;
      _preview_rgb_pixel(detail->preview, src, after,
                         pixels + 3 * ((size_t)y * w + x));
    }
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB,
                                                FALSE, 8, w, h, 3 * w,
                                                _pixbuf_release, NULL);
  if(!pixbuf) g_free(pixels);
  return pixbuf;
}

static gboolean _detail_redraw(_preview_detail_t *detail)
{
  GdkPixbuf *before = _detail_pixbuf(detail, FALSE);
  GdkPixbuf *after = _detail_pixbuf(detail, TRUE);
  if(!before || !after)
  {
    if(before) g_object_unref(before);
    if(after) g_object_unref(after);
    return FALSE;
  }
  gtk_image_set_from_pixbuf(GTK_IMAGE(detail->before), before);
  gtk_image_set_from_pixbuf(GTK_IMAGE(detail->after), after);
  g_object_unref(before);
  g_object_unref(after);
  char location[128];
  g_snprintf(location, sizeof(location), _("full-resolution preview pixels x %d-%d, y %d-%d"),
             detail->preview->x + detail->x,
             detail->preview->x + detail->x + detail->width - 1,
             detail->preview->y + detail->y,
             detail->preview->y + detail->y + detail->height - 1);
  gtk_label_set_text(GTK_LABEL(detail->position), location);
  return TRUE;
}

static void _detail_pan(GtkButton *button, GtkWidget *dialog)
{
  _preview_detail_t *detail = g_object_get_data(G_OBJECT(dialog), "magiceraser-detail");
  if(!detail || gtk_widget_in_destruction(dialog)) return;
  const int direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
  const int old_x = detail->x, old_y = detail->y;
  const int step_x = MAX(1, detail->width / 2);
  const int step_y = MAX(1, detail->height / 2);
  if(direction == 1) detail->x -= step_x;
  if(direction == 2) detail->x += step_x;
  if(direction == 3) detail->y -= step_y;
  if(direction == 4) detail->y += step_y;
  detail->x = CLAMP(detail->x, 0, detail->preview->width - detail->width);
  detail->y = CLAMP(detail->y, 0, detail->preview->height - detail->height);
  if(!_detail_redraw(detail))
  {
    detail->x = old_x;
    detail->y = old_y;
  }
}

static void _detail_dialog_destroyed(GtkWidget *dialog, gboolean *destroyed)
{
  *destroyed = TRUE;
}

static void _detail_boundary_center(const dt_magiceraser_preview_t *p,
                                    int *center_x, int *center_y)
{
  int top = p->height, bottom = -1;
  for(int y = 0; y < p->height; y++)
    for(int x = 0; x < p->width; x++)
      if(p->coverage[(size_t)y * p->width + x] > 0.0f)
      {
        top = MIN(top, y);
        bottom = MAX(bottom, y);
        break;
      }
  *center_x = p->width / 2;
  *center_y = p->height / 2;
  if(bottom < 0) return;
  const int middle = top + (bottom - top) / 2;
  for(int distance = 0; distance < p->height; distance++)
  {
    const int y = middle + (distance & 1 ? -(distance + 1) / 2 : distance / 2);
    if(y < top || y > bottom) continue;
    for(int x = 0; x < p->width; x++)
      if(p->coverage[(size_t)y * p->width + x] > 0.0f)
      {
        *center_x = x;
        *center_y = y;
        return;
      }
  }
}

static gboolean _accept_preview(dt_iop_module_t *self,
                                const dt_magiceraser_preview_t *p,
                                GBytes *digest)
{
  if(!dt_dtdata_enabled() || !_preview_matches(self, p, digest)) return FALSE;
  char source_now[DT_DTDATA_SOURCE_SHA256_LEN] = { 0 };
  if(!dt_dtdata_source_sha256(p->imgid, source_now)
     || strcmp(source_now, p->source_sha256)) return FALSE;

  int x, y, width, height;
  float *rgba = dt_inpaint_extract_patch(p->filled_rgb, p->coverage,
                                         p->width, p->height,
                                         &x, &y, &width, &height);
  if(!rgba) return FALSE;
  const gint64 patch_x = (gint64)p->x + x;
  const gint64 patch_y = (gint64)p->y + y;
  if(patch_x < 0 || patch_x > G_MAXINT || patch_y < 0 || patch_y > G_MAXINT)
  {
    g_free(rgba);
    return FALSE;
  }
  /* Validate placement, source and geometry before writing an authoritative
     entry. A rejected proposal must leave the sidecar untouched. The writer
     supplies only the content-addressed entry name after this check. */
  dt_dtdata_ref_t provisional = { 0 };
  g_strlcpy(provisional.entry, "patch-pending.png", sizeof(provisional.entry));
  provisional.kind = DT_DTDATA_KIND_PATCH;
  provisional.origin = DT_DTDATA_ORIGIN_AUTHORITATIVE;
  provisional.width = width;
  provisional.height = height;
  provisional.bpc = 16;
  g_strlcpy(provisional.producer, "magiceraser", sizeof(provisional.producer));
  dt_iop_magiceraser_params_t params = { 0 };
  if(!dt_magiceraser_params_from_patch(&params, &provisional,
                                       (int)patch_x, (int)patch_y,
                                       p->canvas_width, p->canvas_height,
                                       p->source_sha256, self->dev,
                                       self->iop_order)
     || params.geometry_hash != p->geometry_hash)
  {
    g_free(rgba);
    return FALSE;
  }
  dt_dtdata_ref_t ref = { 0 };
  const gboolean written = dt_dtdata_write_rgba(p->imgid, "magiceraser",
                                                 rgba, width, height, 16, &ref);
  g_free(rgba);
  if(!written) return FALSE;
  g_strlcpy(params.entry, ref.entry, sizeof(params.entry));
  memcpy(self->params, &params, sizeof(params));
  dt_dev_add_new_history_item(self->dev, self, TRUE);
  dt_dev_write_history(self->dev);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _show_preview(dt_magiceraser_preview_state_t *state,
                              guint generation,
                              dt_magiceraser_preview_t *p,
                              GBytes *digest)
{
  dt_iop_module_t *self = state->owner;
  if(!self || generation != state->generation || !_preview_matches(self, p, digest))
    return FALSE;
  GdkPixbuf *before = _preview_pixbuf(p, FALSE);
  GdkPixbuf *after = _preview_pixbuf(p, TRUE);
  if(!before || !after)
  {
    if(before) g_object_unref(before);
    if(after) g_object_unref(after);
    return FALSE;
  }
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (_("Magic Eraser preview"),
     GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
     GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
     _("Discard"), GTK_RESPONSE_CANCEL,
     _("Accept removal"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  GtkWidget *notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                     notebook, TRUE, TRUE, 0);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("before")), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("proposed removal")), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), gtk_image_new_from_pixbuf(before), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), gtk_image_new_from_pixbuf(after), 1, 1, 1, 1);
  g_object_unref(before);
  g_object_unref(after);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid, gtk_label_new(_("overview")));

  /* Render only a small native-resolution window of the full preview. The
     first position straddles the selection's left edge near its middle row. */
  _preview_detail_t *detail = g_try_new0(_preview_detail_t, 1);
  if(!detail)
  {
    gtk_widget_destroy(dialog);
    return FALSE;
  }
  g_object_set_data_full(G_OBJECT(dialog), "magiceraser-detail", detail, g_free);
  detail->preview = p;
  detail->width = MIN(384, p->width);
  detail->height = MIN(384, p->height);
  int center_x, center_y;
  _detail_boundary_center(p, &center_x, &center_y);
  detail->x = CLAMP(center_x - detail->width / 2, 0, p->width - detail->width);
  detail->y = CLAMP(center_y - detail->height / 2, 0, p->height - detail->height);
  GtkWidget *detail_grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(detail_grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(detail_grid), 6);
  gtk_container_set_border_width(GTK_CONTAINER(detail_grid), 12);
  detail->before = gtk_image_new();
  detail->after = gtk_image_new();
  detail->position = gtk_label_new(NULL);
  gtk_grid_attach(GTK_GRID(detail_grid), gtk_label_new(_("before, full resolution")), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(detail_grid), gtk_label_new(_("proposed removal, full resolution")), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(detail_grid), detail->before, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(detail_grid), detail->after, 1, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(detail_grid), detail->position, 0, 2, 2, 1);
  gtk_grid_attach(GTK_GRID(detail_grid),
                  gtk_label_new(_("display scaling may change the screen size")),
                  0, 3, 2, 1);
  GtkWidget *pan = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  const char *pan_labels[] = { N_("pan left"), N_("pan right"),
                               N_("pan up"), N_("pan down") };
  for(int i = 0; i < 4; i++)
  {
    GtkWidget *button = gtk_button_new_with_label(_(pan_labels[i]));
    g_object_set_data(G_OBJECT(button), "direction", GINT_TO_POINTER(i + 1));
    g_signal_connect_object(button, "clicked", G_CALLBACK(_detail_pan),
                            G_OBJECT(dialog), 0);
    gtk_box_pack_start(GTK_BOX(pan), button, FALSE, FALSE, 0);
  }
  gtk_grid_attach(GTK_GRID(detail_grid), pan, 0, 4, 2, 1);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), detail_grid,
                           gtk_label_new(_("full-resolution seam")));
  if(!_detail_redraw(detail))
  {
    gtk_widget_destroy(dialog);
    return FALSE;
  }
  gtk_widget_show_all(dialog);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
  gboolean dialog_destroyed = FALSE;
  const gulong destroy_handler = g_signal_connect_data
    (dialog, "destroy", G_CALLBACK(_detail_dialog_destroyed), &dialog_destroyed,
     NULL, 0);
  g_object_ref(dialog);
  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  const gboolean completed = !dialog_destroyed;
  if(completed) gtk_widget_destroy(dialog);
  if(g_signal_handler_is_connected(dialog, destroy_handler))
    g_signal_handler_disconnect(dialog, destroy_handler);
  g_object_unref(dialog);
  /* The nested GTK loop can switch images or tear down this module. */
  if(completed && response == GTK_RESPONSE_ACCEPT && state->owner == self
     && state->generation == generation)
  {
    if(_accept_preview(self, p, digest))
      dt_toast_log(_("removal saved; use Undo to restore it"));
    else
      dt_toast_log(_("removal not saved: the photo or selection changed"));
  }
  return TRUE;
}

static void _preview_result_free(gpointer user_data)
{
  dt_magiceraser_job_result_t *result = user_data;
  if(result->history_digest) g_bytes_unref(result->history_digest);
  dt_magiceraser_preview_free(result->preview);
  g_free(result->error);
  _preview_state_unref(result->state);
  g_free(result);
}

static gboolean _preview_result_idle(gpointer user_data)
{
  dt_magiceraser_job_result_t *result = user_data;
  dt_magiceraser_preview_state_t *state = result->state;
  state->running = FALSE;
  dt_iop_module_t *self = state->owner;
  dt_iop_magiceraser_gui_data_t *g = self ? self->gui_data : NULL;
  if(g && g->preview_state == state)
    gtk_widget_set_sensitive(g->generate,
      !((dt_iop_magiceraser_params_t *)self->params)->entry[0]);
  if(self && state->generation == result->generation)
  {
    if(g && g->preview_state == state)
    {
      if(result->preview)
      {
        const gboolean shown = _show_preview(state, result->generation,
                                             result->preview,
                                             result->history_digest);
        if(state->owner == self && state->generation == result->generation)
          gtk_label_set_text(GTK_LABEL(g->hint),
            !shown ? _("preview unavailable; removal not saved")
            : ((dt_iop_magiceraser_params_t *)self->params)->entry[0]
              ? _("removal saved; use Undo to restore it")
              : _("preview discarded; selection unchanged"));
      }
      else
        gtk_label_set_text(GTK_LABEL(g->hint), result->error
                           ? result->error : _("could not generate a removal preview"));
    }
  }
  return G_SOURCE_REMOVE;
}

static void _preview_job_params_free(void *user_data)
{
  dt_magiceraser_job_params_t *params = user_data;
  if(params->history_digest) g_bytes_unref(params->history_digest);
  _preview_state_unref(params->state);
  g_free(params);
}

static int32_t _preview_job_run(dt_job_t *job)
{
  dt_magiceraser_job_params_t *params = dt_control_job_get_params(job);
  dt_magiceraser_job_result_t *result = g_new0(dt_magiceraser_job_result_t, 1);
  result->state = params->state;
  g_atomic_int_inc(&result->state->refs);
  result->generation = params->generation;
  result->history_digest = g_bytes_ref(params->history_digest);
  result->preview = dt_magiceraser_preview_generate
    (params->imgid, params->multi_priority, params->history_end,
     params->mask_id, params->group_hash, params->geometry_hash,
     &result->error);
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _preview_result_idle,
                  result, _preview_result_free);
  return 0;
}

static void _generate_preview(GtkButton *button, dt_iop_module_t *self)
{
  if(!self || !self->dev || self->dev != darktable.develop
     || !dt_control_running()) return;
  dt_iop_magiceraser_gui_data_t *g = self->gui_data;
  dt_magiceraser_preview_state_t *state = g ? g->preview_state : NULL;
  if(!state || state->running) return;
  if(!dt_dtdata_enabled())
  {
    gtk_label_set_text(GTK_LABEL(g->hint),
                       _("enable sidecar writing to save a removal"));
    return;
  }
  if(((dt_iop_magiceraser_params_t *)self->params)->entry[0])
  {
    gtk_label_set_text(GTK_LABEL(g->hint),
                       _("paint on a new Magic Eraser instance for another removal"));
    return;
  }
  if(self->dev->form_gui && self->dev->form_gui->creation)
  {
    gtk_label_set_text(GTK_LABEL(g->hint), _("finish painting the brush first"));
    return;
  }
  if(!self->blend_params || !dt_is_valid_maskid(self->blend_params->mask_id))
  {
    gtk_label_set_text(GTK_LABEL(g->hint), _("paint a removal area first"));
    return;
  }

  dt_dev_write_history(self->dev);
  dt_masks_form_t *group = dt_masks_get_from_id(self->dev,
                                                self->blend_params->mask_id);
  GBytes *digest = _history_digest(self->dev->image_storage.id);
  if(!group || !group->points || !digest)
  {
    gtk_label_set_text(GTK_LABEL(g->hint), _("brush selection is not ready"));
    if(digest) g_bytes_unref(digest);
    return;
  }
  dt_magiceraser_job_params_t *params = g_new0(dt_magiceraser_job_params_t, 1);
  params->state = state;
  g_atomic_int_inc(&state->refs);
  params->generation = ++state->generation;
  params->imgid = self->dev->image_storage.id;
  params->multi_priority = self->multi_priority;
  params->history_end = self->dev->history_end;
  params->mask_id = self->blend_params->mask_id;
  params->group_hash = dt_masks_group_hash_ext(DT_INITHASH, group,
                                                self->dev->forms);
  params->geometry_hash = dt_magiceraser_geometry_hash(self->dev,
                                                        self->iop_order);
  params->history_digest = digest;
  if(params->group_hash == DT_INVALID_HASH
     || params->geometry_hash == DT_INVALID_HASH)
  {
    _preview_job_params_free(params);
    gtk_label_set_text(GTK_LABEL(g->hint), _("brush geometry is not ready"));
    return;
  }

  dt_job_t *job = dt_control_job_create(_preview_job_run, "magic eraser preview");
  if(!job)
  {
    _preview_job_params_free(params);
    return;
  }
  state->running = TRUE;
  gtk_widget_set_sensitive(g->generate, FALSE);
  gtk_label_set_text(GTK_LABEL(g->hint), _("finding nearby background…"));
  dt_control_job_set_params(job, params, _preview_job_params_free);
  dt_control_job_add_progress(job, _("generating removal preview"), FALSE);
  if(dt_control_add_job(DT_JOB_QUEUE_USER_BG, job))
  {
    state->running = FALSE;
    gtk_widget_set_sensitive(g->generate, TRUE);
    gtk_label_set_text(GTK_LABEL(g->hint), _("could not queue preview"));
  }
}

static void _paint_selection(GtkButton *button, dt_iop_module_t *self)
{
  if(!self || !self->dev || !self->dev->form_gui) return;
  if(((dt_iop_magiceraser_params_t *)self->params)->entry[0])
  {
    self = dt_iop_gui_duplicate(self, FALSE);
    if(!self) return;
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);
  dt_iop_request_focus(self);
  dt_masks_brush_arm_new_component();
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_BRUSH);
  if(!form) return;
  dt_masks_change_form_gui(form);
  self->dev->form_gui->creation_module = self;
  dt_iop_magiceraser_gui_data_t *g = self->gui_data;
  if(g && g->hint)
    gtk_label_set_text(GTK_LABEL(g->hint),
      _("Experimental Eraser: paint a small area, then preview a nearby fill"));
  dt_control_queue_redraw_center();
}

static void _render_patch_status(dt_iop_module_t *self)
{
  dt_iop_magiceraser_gui_data_t *g = self->gui_data;
  const dt_iop_magiceraser_params_t *p = self->params;
  if(!g || !p || !self->dev) return;

  if(!p->entry[0])
  {
    g->status_valid = FALSE;
    if(g->patch_status_shown)
    {
      gtk_label_set_text(GTK_LABEL(g->hint),
                         _("Experimental Eraser: paint a small area, then preview a nearby fill"));
      gtk_widget_set_tooltip_text(g->hint,
        _("the painted selection does not change the photo until a fill is accepted"));
      g->patch_status_shown = FALSE;
    }
    return;
  }
  g->patch_status_shown = TRUE;
  const char *hint = NULL;
  const char *warning = NULL;
  const char *detail = NULL;
  if(!self->enabled)
    hint = _("saved removal is disabled");
  else if(!g->status_valid
          || g->status_imgid != self->dev->image_storage.id
          || g->status_history_end != self->dev->history_end
          || memcmp(&g->status_params, p, sizeof(*p)))
    hint = _("checking saved removal…");
  else
  {
    dt_iop_magiceraser_patch_status_t status = g->patch_status;
    if(status == DT_MAGICERASER_PATCH_READY)
    {
      const dt_hash_t geometry = dt_magiceraser_geometry_hash(self->dev,
                                                               self->iop_order);
      const dt_hash_t legacy = dt_magiceraser_geometry_hash_legacy(self->dev,
                                                                    self->iop_order);
      if(p->geometry_hash == DT_INVALID_HASH
         || (p->geometry_hash != geometry && p->geometry_hash != legacy))
        status = DT_MAGICERASER_PATCH_GEOMETRY_CHANGED;
    }
    switch(status)
    {
      case DT_MAGICERASER_PATCH_READY:
        hint = _("removal saved; use Undo to restore it");
        break;
      case DT_MAGICERASER_PATCH_GEOMETRY_CHANGED:
        warning = _("saved removal hidden: image geometry changed");
        detail = _("restore the original crop and rotation or create a new removal; "
                   "the source photo is unchanged");
        break;
      case DT_MAGICERASER_PATCH_SOURCE_CHANGED:
        warning = _("saved removal hidden: source photo changed or is unavailable");
        detail = _("the displayed photo may still be cached; restore the original source and restart darktable before trusting its pixels. The accepted removal remains in history");
        break;
      case DT_MAGICERASER_PATCH_MISSING:
        warning = _("saved removal hidden: color patch missing or damaged");
        detail = _("restore the matching .dtdata sidecar and reopen the photo; the source photo is unchanged");
        break;
      default:
        warning = _("saved removal hidden: patch reference is invalid");
        detail = _("the accepted removal remains in history, but cannot be rendered");
        break;
    }
  }

  if(!detail)
    detail = p->entry[0]
      ? _("the accepted fill is stored in the .dtdata sidecar")
      : _("the painted selection does not change the photo until a fill is accepted");
  gtk_label_set_text(GTK_LABEL(g->hint), warning ? warning : hint);
  gtk_widget_set_tooltip_text(g->hint, detail);
}

static void _ui_pipe_finished(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_magiceraser_gui_data_t *g = self->gui_data;
  if(!g || !self->dev || self->dev != darktable.develop
     || !self->dev->full.pipe) return;
  const dt_iop_magiceraser_params_t *p = self->params;
  if(!p || !p->entry[0] || !self->enabled) return;

  dt_dev_pixelpipe_t *pipe = self->dev->full.pipe;
  dt_iop_magiceraser_params_t status_params;
  dt_imgid_t status_imgid = NO_IMGID;
  int32_t status_history_end = -1;
  dt_iop_magiceraser_patch_status_t status = DT_MAGICERASER_PATCH_NONE;
  gboolean found = FALSE;
  dt_pthread_mutex_lock(&pipe->mutex);
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, pipe, self);
  if(piece && piece->enabled && piece->data)
  {
    dt_iop_magiceraser_data_t *d = piece->data;
    g_mutex_lock(&d->patch_lock);
    status_imgid = d->status_imgid;
    status_history_end = d->status_history_end;
    memcpy(&status_params, &d->status_params, sizeof(status_params));
    status = d->runtime_geometry_invalid
      ? DT_MAGICERASER_PATCH_GEOMETRY_CHANGED
      : d->runtime_source_invalid
        ? DT_MAGICERASER_PATCH_SOURCE_CHANGED
        : d->runtime_sidecar_invalid
          ? DT_MAGICERASER_PATCH_MISSING : d->patch_status;
    g_mutex_unlock(&d->patch_lock);
    found = TRUE;
  }
  dt_pthread_mutex_unlock(&pipe->mutex);

  if(!found || status_imgid != self->dev->image_storage.id
     || status_history_end != self->dev->history_end
     || memcmp(&status_params, p, sizeof(status_params))) return;
  g->status_imgid = status_imgid;
  g->status_history_end = status_history_end;
  memcpy(&g->status_params, &status_params, sizeof(status_params));
  g->patch_status = status;
  g->status_valid = TRUE;
  _render_patch_status(self);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_magiceraser_gui_data_t *g = IOP_GUI_ALLOC(magiceraser);
  if(!_source_watch.users++)
    _source_watch.timer = g_timeout_add_seconds(1, _source_watch_tick, NULL);
  _source_watch.have_baseline = FALSE;
  _source_watch.invalidated = FALSE;
  _source_watch.waiting_for_recommit = FALSE;
  g->hint = dt_ui_label_new(_("Experimental Eraser: paint a small area, then preview a nearby fill"));
  gtk_widget_set_tooltip_text(g->hint,
                             _("on large photos, scroll down over the image while painting to shrink the brush; inspect the full-resolution preview before accepting"));
  g->paint = gtk_button_new_with_label(_("paint removal area"));
  gtk_widget_set_tooltip_text(g->paint,
                             _("paint a small area on the image; scroll down over the image to shrink the brush when needed"));
  g_signal_connect(g->paint, "clicked", G_CALLBACK(_paint_selection), self);
  g->preview_state = g_new0(dt_magiceraser_preview_state_t, 1);
  g->preview_state->refs = 1;
  g->preview_state->owner = self;
  g->generate = gtk_button_new_with_label(_("preview nearby fill"));
  g_signal_connect(g->generate, "clicked", G_CALLBACK(_generate_preview), self);
  self->widget = dt_gui_vbox(g->hint, g->paint, g->generate);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _ui_pipe_finished);
}

void gui_update(dt_iop_module_t *self)
{
  _render_patch_status(self);
  dt_iop_magiceraser_gui_data_t *g = self->gui_data;
  if(g && g->preview_state)
  {
    g->preview_state->generation++;
    gtk_widget_set_sensitive(g->generate,
      !g->preview_state->running
      && !((dt_iop_magiceraser_params_t *)self->params)->entry[0]);
  }
}

void gui_cleanup(dt_iop_module_t *self)
{
  if(_source_watch.users && !--_source_watch.users)
  {
    if(_source_watch.timer) g_source_remove(_source_watch.timer);
    _source_watch.timer = 0;
    _source_watch.have_baseline = FALSE;
    _source_watch.invalidated = FALSE;
    _source_watch.waiting_for_recommit = FALSE;
    _take_sidecar_refresh(_source_watch.imgid);
    _source_watch.imgid = NO_IMGID;
  }
  dt_iop_magiceraser_gui_data_t *g = self->gui_data;
  if(g && g->preview_state)
  {
    g->preview_state->owner = NULL;
    g->preview_state->generation++;
    _preview_state_unref(g->preview_state);
    g->preview_state = NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
