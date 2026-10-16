/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <stddef.h>
#include <glib.h>

/* Internal to the Magic Eraser module. The caller supplies the decoded
   encoded-sRGB floats and their actual logical length; no provider is trusted
   to choose or resize the ROI accepted by the preview. */
typedef struct dt_magiceraser_rgb_result_t
{
  int x, y, width, height;
  int channels;
  /* Decoders must declare this explicitly; linear-light RGB is not accepted. */
  enum { DT_MAGICERASER_RGB_UNKNOWN, DT_MAGICERASER_RGB_ENCODED_SRGB } encoding;
  size_t row_stride_floats;
  size_t float_count;
  const float *values;
} dt_magiceraser_rgb_result_t;

/* The destination is untouched on failure. On success it holds unblended
   replacement RGB inside coverage and the exact source outside coverage. */
gboolean dt_magiceraser_result_apply(int roi_x, int roi_y,
                                     int roi_width, int roi_height,
                                     int canvas_width, int canvas_height,
                                     const float *source_rgb,
                                     const float *coverage,
                                     float *filled_rgb,
                                     const dt_magiceraser_rgb_result_t *result);

/* Experimental, bounded background transplant for small painted areas.
   It chooses a clean nearby source region by comparing the pixels around the
   selection. The destination is untouched on failure. The proposed RGB is
   unblended; the saved patch applies coverage exactly once. */
gboolean dt_magiceraser_nearby_fill(const float *source_rgb,
                                    const float *coverage,
                                    int width, int height,
                                    float *proposed_rgb,
                                    char **error);
