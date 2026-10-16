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

#include "common/ai/inpaint.h"

#include <math.h>
#include <string.h>


float *dt_inpaint_extract_patch(const float *filled_rgb,
                                const float *coverage,
                                const int width,
                                const int height,
                                int *patch_x,
                                int *patch_y,
                                int *patch_width,
                                int *patch_height)
{
  if(!filled_rgb || !coverage || !patch_x || !patch_y
     || !patch_width || !patch_height || width <= 0 || height <= 0
     || (size_t)width * height > 16u * 1024u * 1024u)
    return NULL;

  int left = width, top = height, right = -1, bottom = -1;
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      const float a = coverage[i];
      if(!isfinite(a) || a < 0.0f || a > 1.0f) return NULL;
      for(int c = 0; c < 3; c++)
        if(!isfinite(filled_rgb[3 * i + c])
           || filled_rgb[3 * i + c] < 0.0f
           || filled_rgb[3 * i + c] > 1.0f)
          return NULL;
      if(a > 0.0f)
      {
        left = MIN(left, x);
        top = MIN(top, y);
        right = MAX(right, x);
        bottom = MAX(bottom, y);
      }
    }
  if(right < left) return NULL;

  const int w = right - left + 1, h = bottom - top + 1;
  float *rgba = g_try_new(float, 4 * (size_t)w * h);
  if(!rgba) return NULL;
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const size_t src = (size_t)(y + top) * width + x + left;
      const size_t dst = (size_t)y * w + x;
      for(int c = 0; c < 3; c++) rgba[4 * dst + c] = filled_rgb[3 * src + c];
      rgba[4 * dst + 3] = coverage[src];
    }
  *patch_x = left;
  *patch_y = top;
  *patch_width = w;
  *patch_height = h;
  return rgba;
}

gboolean dt_inpaint_soft_mask_from_selection(const float *selection,
                                             const int width,
                                             const int height,
                                             const int padding,
                                             const int feather,
                                             float *coverage,
                                             int *box_x,
                                             int *box_y,
                                             int *box_width,
                                             int *box_height)
{
  if(!selection || !coverage || !box_x || !box_y || !box_width || !box_height
     || width <= 0 || height <= 0 || padding < 0 || feather < 0
     || (size_t)width * height > 16u * 1024u * 1024u)
    return FALSE;

  const size_t pixels = (size_t)width * height;
  guint16 *distance = g_try_new(guint16, pixels);
  if(!distance) return FALSE;

  gboolean selected = FALSE;
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      const float value = selection[i];
      if(!isfinite(value) || value < 0.0f || value > 1.0f)
      {
        g_free(distance);
        return FALSE;
      }
      distance[i] = value >= 0.5f ? 0 : G_MAXUINT16 / 2;
      selected |= value >= 0.5f;
    }
  if(!selected)
  {
    g_free(distance);
    return FALSE;
  }

  /* The painted shape, rather than its bounding rectangle, determines what
     can change. Two Manhattan-distance passes add a small safety margin and
     a feather without filling the empty space between separate strokes. */
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      if(x) distance[i] = MIN(distance[i], distance[i - 1] + 1);
      if(y) distance[i] = MIN(distance[i], distance[i - width] + 1);
    }
  for(int y = height - 1; y >= 0; y--)
    for(int x = width - 1; x >= 0; x--)
    {
      const size_t i = (size_t)y * width + x;
      if(x + 1 < width) distance[i] = MIN(distance[i], distance[i + 1] + 1);
      if(y + 1 < height) distance[i] = MIN(distance[i], distance[i + width] + 1);
    }

  int left = width, top = height, right = -1, bottom = -1;
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      const int d = distance[i];
      const float alpha = d <= padding ? 1.0f
        : feather && d <= padding + feather
          ? (float)(padding + feather + 1 - d) / (feather + 1) : 0.0f;
      coverage[i] = alpha;
      if(alpha > 0.0f)
      {
        left = MIN(left, x);
        top = MIN(top, y);
        right = MAX(right, x);
        bottom = MAX(bottom, y);
      }
    }
  g_free(distance);
  if(right < left) return FALSE;
  *box_x = left;
  *box_y = top;
  *box_width = right - left + 1;
  *box_height = bottom - top + 1;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
