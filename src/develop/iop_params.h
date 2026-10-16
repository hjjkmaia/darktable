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

// JSON-free builder for iop params blobs, extracted from the MCP bridge's
// _seed_and_apply() so the Lightroom import can seed module defaults and set
// scalar fields by introspection name without going through JSON.
// A builder is used by one thread; the API keeps no globals.

typedef struct dt_iop_params_builder_t dt_iop_params_builder_t;

typedef enum dt_iop_params_err_t
{
  DT_IOP_PARAMS_OK = 0,
  DT_IOP_PARAMS_NO_MODULE,         // unknown op
  DT_IOP_PARAMS_NO_INTROSPECTION,  // module has no introspection
  DT_IOP_PARAMS_SIZE_MISMATCH,     // defaults size != introspection size
  DT_IOP_PARAMS_NO_FIELD,          // unknown field name
  DT_IOP_PARAMS_TYPE,              // field is not a scalar of that type
  DT_IOP_PARAMS_RANGE,             // value outside the field's hard range
} dt_iop_params_err_t;

// defaults: caller-owned copy source (e.g. module->default_params); it is
// copied, so the caller may free it afterwards. NULL means "synthesize from
// $DEFAULT", refused with DT_IOP_PARAMS_TYPE for modules with non-scalar
// fields
dt_iop_params_builder_t *dt_iop_params_builder_new(const char *op,
                                                   const void *defaults,
                                                   size_t defaults_size,
                                                   dt_iop_params_err_t *err);
dt_iop_params_err_t dt_iop_params_builder_set_float(dt_iop_params_builder_t *b,
                                                    const char *field, float v);
dt_iop_params_err_t dt_iop_params_builder_set_int(dt_iop_params_builder_t *b,
                                                  const char *field, int v);
dt_iop_params_err_t dt_iop_params_builder_get_float(const dt_iop_params_builder_t *b,
                                                    const char *field, float *v);
dt_iop_params_err_t dt_iop_params_builder_get_int(const dt_iop_params_builder_t *b,
                                                  const char *field, int *v);
// blob stays owned by the builder until _free; version is so->version()
const void *dt_iop_params_builder_blob(const dt_iop_params_builder_t *b,
                                       size_t *size, int *version);
void dt_iop_params_builder_free(dt_iop_params_builder_t *b);
const char *dt_iop_params_err_str(dt_iop_params_err_t err);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
