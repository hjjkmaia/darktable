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

#pragma once

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

// Lightroom (crs:) tag -> darktable module field mapping table, the data side
// of the Lightroom XMP import. Rows are data: the importer walks them, looks
// up the workflow mask, and writes the fields through the params builder
// (develop/iop_params.h). White balance, the parametric curve, and
// sharpness / noise reduction are code in lightroom.c, not rows here.
// A zero noise channel still has to be stored, which SKIP_IF_ZERO cannot do.

typedef enum dt_lr_workflow_t
{
  DT_LR_WF_SIGMOID = 1 << 0,
  DT_LR_WF_FILMIC  = 1 << 1,
  DT_LR_WF_AGX     = 1 << 2,
  DT_LR_WF_OTHER   = 1 << 3,  // spektrafilm, display-referred, none
  DT_LR_WF_ALL     = 0xf,
} dt_lr_workflow_t;

typedef enum dt_lr_flags_t
{
  DT_LR_ADD_TO_DEFAULT = 1 << 0,  // new value = module default + converted value
  DT_LR_SKIP_IF_ZERO   = 1 << 1,  // a zero LR value contributes nothing
} dt_lr_flags_t;

typedef struct dt_lr_mapping_t
{
  const char *crs_tag;           // e.g. "Highlights2012"
  const char *op;                // e.g. "contrastntexture"
  const char *field;             // introspection field, e.g. "gain_highlights"
  float (*convert)(float lr);    // LR value -> darktable value (before ADD_TO_DEFAULT)
  uint32_t flags;                // dt_lr_flags_t
  uint32_t workflows;            // dt_lr_workflow_t mask
} dt_lr_mapping_t;

// all rows whose crs_tag matches and whose mask contains wf (several rows
// per tag are allowed: one tag can write several fields)
size_t dt_lr_map_lookup(const char *crs_tag, dt_lr_workflow_t wf,
                        const dt_lr_mapping_t **rows, size_t max_rows);

// map a plugins/darkroom/workflow conf value to a workflow bit
dt_lr_workflow_t dt_lr_workflow_from_conf(const char *workflow_conf_value);

// resolve every row's op and field through the module's introspection;
// FALSE with a message naming the bad row on the first failure
gboolean dt_lr_map_validate(char **err);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
