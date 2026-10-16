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

#pragma once

#include <glib.h>


/** Crop a proposed fill and its coverage to an RGBA patch for .dtdata.
 *  The returned RGB is encoded sRGB and alpha is coverage; caller frees it
 *  with g_free. Return NULL for empty or invalid input, leaving bounds
 *  untouched. The caller records these bounds relative to the full canvas
 *  and writes the patch only after explicit user acceptance. */
float *dt_inpaint_extract_patch(const float *filled_rgb,
                                const float *coverage,
                                int width,
                                int height,
                                int *patch_x,
                                int *patch_y,
                                int *patch_width,
                                int *patch_height);

/** Expand the painted selection by padding pixels, then feather its edge.
 *  Separate brush strokes stay separate rather than filling their bounding
 *  rectangle. Pixels outside the returned bounds have zero coverage. On
 *  failure the caller's output and bounds are untouched. */
gboolean dt_inpaint_soft_mask_from_selection(const float *selection,
                                             int width,
                                             int height,
                                             int padding,
                                             int feather,
                                             float *coverage,
                                             int *box_x,
                                             int *box_y,
                                             int *box_width,
                                             int *box_height);
