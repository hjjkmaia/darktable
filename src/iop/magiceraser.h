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

#include "common/darktable.h"
#include "common/dtdata.h"
#include "develop/develop.h"

#include <string.h>

/* Kept in a shared header so the acceptance UI can construct the same
   history parameters as the pixelpipe consumer. One instance stores one
   accepted fill; the pixels themselves live in the authoritative .dtdata
   sidecar, never in the image file. */
typedef struct dt_iop_magiceraser_params_t
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
  /* Bind a color patch to the source photo bytes, not its catalog ID or
     path. History copied to a different same-size photo must fail closed. */
  char source_sha256[DT_DTDATA_SOURCE_SHA256_LEN];
} dt_iop_magiceraser_params_t;

static inline gboolean dt_magiceraser_valid_sha256(const char *const value)
{
  if(!value) return FALSE;
  for(int i = 0; i < DT_DTDATA_SOURCE_SHA256_LEN - 1; i++)
    if(!((value[i] >= '0' && value[i] <= '9')
         || (value[i] >= 'a' && value[i] <= 'f')))
      return FALSE;
  return value[DT_DTDATA_SOURCE_SHA256_LEN - 1] == '\0';
}

/* The patch is in the input coordinates of this module. Hash upstream
   geometry in pipeline order, but not absolute order numbers: inserting a
   non-distorting module can renumber every module without moving a pixel. */
static inline dt_hash_t _dt_magiceraser_geometry_hash(const dt_develop_t *const dev,
                                                       const int eraser_order,
                                                       const gboolean legacy_order)
{
  if(!dev || eraser_order <= 0) return DT_INVALID_HASH;
  dt_hash_t hash = DT_INITHASH;
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    const dt_iop_module_t *const mod = m->data;
    if(mod->iop_order >= eraser_order
       || !(mod->operation_tags() & IOP_TAG_DISTORT))
      continue;

    const dt_dev_history_item_t *last = NULL;
    int n = 0;
    for(GList *h = dev->history; h && n < dev->history_end;
        h = g_list_next(h), n++)
    {
      const dt_dev_history_item_t *const item = h->data;
      if(item->module == mod) last = item;
    }
    const gboolean enabled = last ? last->enabled : mod->default_enabled;
    if(!enabled) continue;
    const void *const params = last ? (const void *)last->params
                                    : (const void *)mod->default_params;
    if(!params || mod->params_size <= 0) return DT_INVALID_HASH;
    hash = dt_hash(hash, mod->op, strlen(mod->op));
    hash = dt_hash(hash, &mod->multi_priority, sizeof(mod->multi_priority));
    if(legacy_order)
      hash = dt_hash(hash, &mod->iop_order, sizeof(mod->iop_order));
    hash = dt_hash(hash, params, mod->params_size);
  }
  return hash;
}

static inline dt_hash_t dt_magiceraser_geometry_hash(const dt_develop_t *const dev,
                                                      const int eraser_order)
{
  return _dt_magiceraser_geometry_hash(dev, eraser_order, FALSE);
}

/* Prototype patches stored before the relative-order guard used absolute
   order numbers. Continue reading them when their old fingerprint matches. */
static inline dt_hash_t dt_magiceraser_geometry_hash_legacy(const dt_develop_t *const dev,
                                                             const int eraser_order)
{
  return _dt_magiceraser_geometry_hash(dev, eraser_order, TRUE);
}

/* Validate patch metadata and placement before recording a history step.
   Acceptance may use a provisional entry name before the sidecar write, then
   replace that name with the writer's content-addressed entry. */
static inline gboolean dt_magiceraser_params_from_patch(
    dt_iop_magiceraser_params_t *const params,
    const dt_dtdata_ref_t *const ref,
    const int patch_x,
    const int patch_y,
    const int canvas_width,
    const int canvas_height,
    const char proposal_source_sha256[DT_DTDATA_SOURCE_SHA256_LEN],
    const dt_develop_t *const dev,
    const int eraser_order)
{
  if(!params || !ref || !dev || !ref->entry[0]
     || !memchr(ref->entry, '\0', sizeof(ref->entry))
     || !memchr(ref->producer, '\0', sizeof(ref->producer))
     || ref->kind != DT_DTDATA_KIND_PATCH
     || ref->origin != DT_DTDATA_ORIGIN_AUTHORITATIVE
     || (ref->bpc != 8 && ref->bpc != 16)
     || ref->width <= 0 || ref->height <= 0
     || patch_x < 0 || patch_y < 0
     || canvas_width <= 0 || canvas_height <= 0
     || patch_x > canvas_width - ref->width
     || patch_y > canvas_height - ref->height
     || !dt_magiceraser_valid_sha256(proposal_source_sha256))
    return FALSE;

  const dt_hash_t geometry_hash = dt_magiceraser_geometry_hash(dev, eraser_order);
  char current_sha256[DT_DTDATA_SOURCE_SHA256_LEN] = { 0 };
  if(geometry_hash == DT_INVALID_HASH
     || !dt_dtdata_source_sha256(dev->image_storage.id, current_sha256)
     || strcmp(proposal_source_sha256, current_sha256))
    return FALSE;

  memset(params, 0, sizeof(*params));
  g_strlcpy(params->entry, ref->entry, sizeof(params->entry));
  params->entry_kind = ref->kind;
  params->entry_origin = ref->origin;
  params->entry_width = ref->width;
  params->entry_height = ref->height;
  params->entry_bpc = ref->bpc;
  g_strlcpy(params->entry_producer, ref->producer,
            sizeof(params->entry_producer));
  params->patch_x = patch_x;
  params->patch_y = patch_y;
  params->canvas_width = canvas_width;
  params->canvas_height = canvas_height;
  params->geometry_hash = geometry_hash;
  g_strlcpy(params->source_sha256, current_sha256, sizeof(params->source_sha256));
  return TRUE;
}
