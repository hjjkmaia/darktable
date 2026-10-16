/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "common/darktable.h"
#include "develop/develop.h"

typedef struct dt_magiceraser_preview_t
{
  dt_imgid_t imgid;
  int multi_priority;
  int32_t history_end;
  dt_mask_id_t mask_id;
  dt_hash_t geometry_hash;
  dt_hash_t group_hash;
  int canvas_width;
  int canvas_height;
  int x;
  int y;
  int width;
  int height;
  char source_sha256[65];
  /* All buffers use the ROI dimensions. RGB is encoded sRGB; coverage is
     applied once, in both the preview and the accepted pixelpipe patch. */
  float *source_rgb;
  float *filled_rgb;
  float *coverage;
  gboolean result_validated;
} dt_magiceraser_preview_t;

void dt_magiceraser_preview_free(dt_magiceraser_preview_t *preview);

/* Called only in a background job with its own develop context. The caller
   must have flushed the live history and must recheck it before accepting. */
dt_magiceraser_preview_t *dt_magiceraser_preview_generate(
    dt_imgid_t imgid, int multi_priority, int32_t history_end,
    dt_mask_id_t mask_id, dt_hash_t group_hash, dt_hash_t geometry_hash,
    char **error);
