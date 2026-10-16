/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "iop/magiceraser_result.h"

#include <math.h>
#include <string.h>

#define DT_ERASER_MAX_RESULT_PIXELS (2048u * 2048u)
#define DT_ERASER_NEARBY_MAX_SIDE 512
#define DT_ERASER_NEARBY_MAX_SUPPORT 96
#define DT_ERASER_NEARBY_SEARCH 160
#define DT_ERASER_NEARBY_RING 8

static gboolean _nearby_error(char **error, const char *message)
{
  if(error && !*error) *error = g_strdup(message);
  return FALSE;
}

static guint32 _covered_in_rect(const guint32 *sum, const int stride,
                                const int left, const int top,
                                const int right, const int bottom)
{
  return sum[(size_t)bottom * stride + right]
         - sum[(size_t)top * stride + right]
         - sum[(size_t)bottom * stride + left]
         + sum[(size_t)top * stride + left];
}

/* All rectangles use exclusive right/bottom coordinates. A donor must keep
   the entire painted area and its comparison ring clear of the selection. */
static double _donor_score(const float *rgb, const guint32 *sum,
                           const int width, const int height,
                           const int left, const int top,
                           const int right, const int bottom,
                           const int dx, const int dy)
{
  const int margin = DT_ERASER_NEARBY_RING;
  if(left + dx < margin || top + dy < margin
     || right + dx + margin > width || bottom + dy + margin > height
     || _covered_in_rect(sum, width + 1,
                         left + dx - margin, top + dy - margin,
                         right + dx + margin, bottom + dy + margin))
    return HUGE_VAL;

  double difference = 0.0;
  guint samples = 0;
  for(int y = MAX(0, top - margin); y < MIN(height, bottom + margin); y += 2)
    for(int x = MAX(0, left - margin); x < MIN(width, right + margin); x += 2)
    {
      if(x >= left && x < right && y >= top && y < bottom) continue;
      const int donor_x = x + dx, donor_y = y + dy;
      if(donor_x < 0 || donor_x >= width || donor_y < 0 || donor_y >= height)
        continue;
      const size_t a = 3 * ((size_t)y * width + x);
      const size_t b = 3 * ((size_t)donor_y * width + donor_x);
      for(int c = 0; c < 3; c++)
      {
        const double delta = (double)rgb[a + c] - rgb[b + c];
        difference += delta * delta;
      }
      samples++;
    }
  if(samples < 16) return HUGE_VAL;
  /* Prefer a closer donor only when boundary colors are effectively tied. */
  return difference / (3.0 * samples)
         + 0.0000001 * ((double)dx * dx + (double)dy * dy);
}

gboolean dt_magiceraser_nearby_fill(const float *source_rgb,
                                    const float *coverage,
                                    const int width, const int height,
                                    float *proposed_rgb,
                                    char **error)
{
  if(error) *error = NULL;
  if(!source_rgb || !coverage || !proposed_rgb
     || width <= 0 || height <= 0
     || width > DT_ERASER_NEARBY_MAX_SIDE
     || height > DT_ERASER_NEARBY_MAX_SIDE)
    return _nearby_error(error, "preview area is too large for Experimental Eraser");

  const size_t pixels = (size_t)width * height;
  const size_t stride = (size_t)width + 1;
  guint32 *sum = g_try_new0(guint32, stride * ((size_t)height + 1));
  if(!sum) return _nearby_error(error, "out of memory");
  int left = width, top = height, right = -1, bottom = -1;
  guint covered = 0;
  for(int y = 0; y < height; y++)
  {
    guint32 row = 0;
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      const float a = coverage[i];
      if(!isfinite(a) || a < 0.0f || a > 1.0f)
      {
        g_free(sum);
        return _nearby_error(error, "the painted coverage is invalid");
      }
      for(int c = 0; c < 3; c++)
        if(!isfinite(source_rgb[3 * i + c])
           || source_rgb[3 * i + c] < 0.0f
           || source_rgb[3 * i + c] > 1.0f)
        {
          g_free(sum);
          return _nearby_error(error, "the source pixels are invalid");
        }
      if(a > 0.0f)
      {
        row++;
        covered++;
        left = MIN(left, x);
        top = MIN(top, y);
        right = MAX(right, x);
        bottom = MAX(bottom, y);
      }
      sum[(size_t)(y + 1) * stride + x + 1]
        = sum[(size_t)y * stride + x + 1] + row;
    }
  }
  if(!covered)
  {
    g_free(sum);
    return _nearby_error(error, "paint a small area before making a preview");
  }
  right++;
  bottom++;
  if(right - left > DT_ERASER_NEARBY_MAX_SUPPORT
     || bottom - top > DT_ERASER_NEARBY_MAX_SUPPORT
     || covered > DT_ERASER_NEARBY_MAX_SUPPORT * DT_ERASER_NEARBY_MAX_SUPPORT)
  {
    g_free(sum);
    return _nearby_error(error, "painted area is too large; shrink the brush with scroll and repaint");
  }

  double best = HUGE_VAL;
  int best_dx = 0, best_dy = 0;
  for(int dy = -DT_ERASER_NEARBY_SEARCH; dy <= DT_ERASER_NEARBY_SEARCH; dy += 4)
    for(int dx = -DT_ERASER_NEARBY_SEARCH; dx <= DT_ERASER_NEARBY_SEARCH; dx += 4)
    {
      if(!dx && !dy) continue;
      const double score = _donor_score(source_rgb, sum, width, height,
                                        left, top, right, bottom, dx, dy);
      if(score < best)
      {
        best = score;
        best_dx = dx;
        best_dy = dy;
      }
    }
  g_free(sum);
  /* A visibly different border is more likely to be an object or edge than
     usable background. Let the user inspect borderline fills before Accept. */
  if(!isfinite(best) || best > 0.015)
    return _nearby_error(error, "no sufficiently similar nearby background was found");

  memcpy(proposed_rgb, source_rgb, 3 * pixels * sizeof(float));
  for(int y = top; y < bottom; y++)
    for(int x = left; x < right; x++)
    {
      const size_t dst = (size_t)y * width + x;
      if(coverage[dst] <= 0.0f) continue;
      const size_t donor = (size_t)(y + best_dy) * width + x + best_dx;
      memcpy(proposed_rgb + 3 * dst, source_rgb + 3 * donor,
             3 * sizeof(float));
    }
  return TRUE;
}

gboolean dt_magiceraser_result_apply(const int roi_x, const int roi_y,
                                     const int roi_width, const int roi_height,
                                     const int canvas_width, const int canvas_height,
                                     const float *source_rgb,
                                     const float *coverage,
                                     float *filled_rgb,
                                     const dt_magiceraser_rgb_result_t *result)
{
  if(!source_rgb || !coverage || !filled_rgb || !result || !result->values
     || roi_x < 0 || roi_y < 0 || roi_width <= 0 || roi_height <= 0
     || canvas_width <= 0 || canvas_height <= 0
     || roi_width > canvas_width || roi_height > canvas_height
     || roi_x > canvas_width - roi_width
     || roi_y > canvas_height - roi_height
     || result->x != roi_x || result->y != roi_y
     || result->width != roi_width || result->height != roi_height
     || result->channels != 3
     || result->encoding != DT_MAGICERASER_RGB_ENCODED_SRGB
     || (size_t)roi_width > DT_ERASER_MAX_RESULT_PIXELS / (size_t)roi_height)
    return FALSE;

  const size_t pixels = (size_t)roi_width * roi_height;
  if(result->row_stride_floats != 3 * (size_t)roi_width
     || result->float_count != 3 * pixels)
    return FALSE;

  gboolean covered = FALSE;
  for(size_t i = 0; i < pixels; i++)
  {
    const float a = coverage[i];
    if(!isfinite(a) || a < 0.0f || a > 1.0f) return FALSE;
    covered |= a > 0.0f;
    for(int c = 0; c < 3; c++)
    {
      const float source = source_rgb[3 * i + c];
      const float proposed = result->values[3 * i + c];
      if(!isfinite(source) || source < 0.0f || source > 1.0f
         || !isfinite(proposed) || proposed < 0.0f || proposed > 1.0f)
        return FALSE;
    }
  }
  if(!covered) return FALSE;

  for(size_t i = 0; i < pixels; i++)
    memcpy(filled_rgb + 3 * i,
           (coverage[i] > 0.0f ? result->values : source_rgb) + 3 * i,
           3 * sizeof(float));
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
