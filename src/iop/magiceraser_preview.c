/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "iop/magiceraser_preview.h"

#include "common/ai/inpaint.h"
#include "common/dtdata.h"
#include "common/iop_profile.h"
#include "common/mipmap_cache.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_common.h"
#include "iop/magiceraser.h"
#include "iop/magiceraser_result.h"

#include <math.h>
#include <string.h>

/* One preview is bounded. A larger selected region must be tiled explicitly;
   shrinking it would break the saved patch's full-resolution coordinates. */
#define DT_ERASER_MAX_PREVIEW_PIXELS (512u * 512u)
#define DT_ERASER_NEARBY_CONTEXT 160
#define DT_ERASER_MAX_MASK_DEPTH 8
#define DT_ERASER_MAX_MASK_FORMS 256

typedef struct dt_eraser_selection_bounds_t
{
  gint64 left, top, right, bottom;
  guint forms;
  gboolean has_difference;
} dt_eraser_selection_bounds_t;

void dt_magiceraser_preview_free(dt_magiceraser_preview_t *p)
{
  if(!p) return;
  g_free(p->source_rgb);
  g_free(p->filled_rgb);
  g_free(p->coverage);
  g_free(p);
}

static dt_magiceraser_preview_t *_error(dt_magiceraser_preview_t *p,
                                        char **error,
                                        const char *message)
{
  if(error && !*error) *error = g_strdup(message);
  dt_magiceraser_preview_free(p);
  return NULL;
}

/* Validate every child before group rendering: the group rasterizer accepts a
   partial result when one child, including a subtractive hole, fails. */
static gboolean _selection_walk(dt_iop_module_t *module,
                                dt_dev_pixelpipe_iop_t *piece,
                                dt_masks_form_t *form,
                                const gboolean difference,
                                const float difference_opacity,
                                dt_mask_id_t ancestors[DT_ERASER_MAX_MASK_DEPTH],
                                const guint depth,
                                dt_eraser_selection_bounds_t *bounds,
                                const dt_iop_roi_t *roi,
                                float *scratch,
                                float *holes)
{
  if(!form || !form->points || !dt_is_valid_maskid(form->formid)
     || (form->type & DT_MASKS_CLONE)
     || depth >= DT_ERASER_MAX_MASK_DEPTH
     || ++bounds->forms > DT_ERASER_MAX_MASK_FORMS)
    return FALSE;
  for(guint i = 0; i < depth; i++)
    if(ancestors[i] == form->formid) return FALSE;
  ancestors[depth] = form->formid;

  if(form->type == DT_MASKS_GROUP)
  {
    if(difference) return FALSE;
    for(GList *it = form->points; it; it = g_list_next(it))
    {
      const dt_masks_point_group_t *child = it->data;
      if(!child || child->parentid != form->formid
         || !isfinite(child->opacity) || child->opacity < 0.0f
         || child->opacity > 1.0f
         || (child->state & ~(DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE
                              | DT_MASKS_STATE_UNION | DT_MASKS_STATE_SUM
                              | DT_MASKS_STATE_DIFFERENCE)))
        return FALSE;
      const int op = child->state & DT_MASKS_STATE_OP;
      if(op && op != DT_MASKS_STATE_UNION && op != DT_MASKS_STATE_SUM
         && op != DT_MASKS_STATE_DIFFERENCE)
        return FALSE;
      dt_masks_form_t *next = dt_masks_get_from_id_ext(piece->pipe->forms,
                                                        child->formid);
      if(!next || (op == DT_MASKS_STATE_DIFFERENCE
                   && next->type != DT_MASKS_PATH
                   && next->type != DT_MASKS_BRUSH))
        return FALSE;
      if(op == DT_MASKS_STATE_DIFFERENCE) bounds->has_difference = TRUE;
      if(!_selection_walk(module, piece, next,
                          op == DT_MASKS_STATE_DIFFERENCE,
                          child->opacity,
                          ancestors, depth + 1, bounds, roi, scratch, holes))
        return FALSE;
    }
    return TRUE;
  }

  if(form->type != DT_MASKS_BRUSH && form->type != DT_MASKS_PATH) return FALSE;
  int x, y, w, h;
  if(!dt_masks_get_area(module, piece, form, &w, &h, &x, &y)
     || w <= 0 || h <= 0)
    return FALSE;
  if(!difference)
  {
    bounds->left = MIN(bounds->left, (gint64)x);
    bounds->top = MIN(bounds->top, (gint64)y);
    bounds->right = MAX(bounds->right, (gint64)x + w);
    bounds->bottom = MAX(bounds->bottom, (gint64)y + h);
  }
  if(roi)
  {
    const size_t pixels = (size_t)roi->width * roi->height;
    memset(scratch, 0, pixels * sizeof(float));
    if(!dt_masks_get_mask_roi(module, piece, form, roi, scratch)) return FALSE;
    for(size_t i = 0; i < pixels; i++)
    {
      if(!isfinite(scratch[i]) || scratch[i] < 0.0f
         || scratch[i] > 1.0f) return FALSE;
      if(difference) holes[i] = MAX(holes[i], difference_opacity * scratch[i]);
    }
  }
  return TRUE;
}

/* The fill is a bounded translation of clean nearby source pixels. Keep its
   raw proposal separate from coverage, then validate exact ROI semantics. */
static gboolean _run_nearby_fill(dt_magiceraser_preview_t *p, char **error)
{
  const size_t n = (size_t)p->width * p->height;
  float *proposal = g_try_new(float, 3 * n);
  if(!proposal)
  {
    if(error && !*error) *error = g_strdup("out of memory");
    return FALSE;
  }
  gboolean ok = dt_magiceraser_nearby_fill(p->source_rgb, p->coverage,
                                           p->width, p->height, proposal, error);
  if(ok)
  {
    const dt_magiceraser_rgb_result_t result = {
      p->x, p->y, p->width, p->height, 3, DT_MAGICERASER_RGB_ENCODED_SRGB,
      3 * (size_t)p->width, 3 * n, proposal
    };
    ok = dt_magiceraser_result_apply(p->x, p->y, p->width, p->height,
                                     p->canvas_width, p->canvas_height,
                                     p->source_rgb, p->coverage,
                                     p->filled_rgb, &result);
    p->result_validated = ok;
    if(!ok && error && !*error)
      *error = g_strdup("the proposed fill was invalid");
  }
  g_free(proposal);
  return ok;
}

static dt_magiceraser_preview_t *_prepare_preview(
    const dt_imgid_t imgid, const int multi_priority, const int32_t history_end,
    const dt_mask_id_t mask_id, const dt_hash_t group_hash,
    const dt_hash_t geometry_hash, char **error)
{
  if(!dt_is_valid_imgid(imgid) || !dt_is_valid_maskid(mask_id)
     || geometry_hash == DT_INVALID_HASH || group_hash == DT_INVALID_HASH)
    return _error(NULL, error, "selection is unavailable");

  dt_magiceraser_preview_t *p = g_try_new0(dt_magiceraser_preview_t, 1);
  if(!p) return _error(NULL, error, "out of memory");
  p->imgid = imgid;
  p->multi_priority = multi_priority;
  p->history_end = history_end;
  p->mask_id = mask_id;
  p->group_hash = group_hash;
  p->geometry_hash = geometry_hash;
  if(!dt_dtdata_source_sha256(imgid, p->source_sha256))
    return _error(p, error, "could not verify the source photo");

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  if(dev.image_storage.id != imgid || dev.history_end != history_end)
  {
    dt_dev_cleanup(&dev);
    return _error(p, error, "photo history changed before preview");
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf || buf.width <= 0 || buf.height <= 0)
  {
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "could not read the photo");
  }

  dt_dev_pixelpipe_t pipe;
  /* The export cache grows for the ROI; a full RAW-size hint would reserve
     two enormous cachelines before any pixels are rendered. */
  if(!dt_dev_pixelpipe_init_export(&pipe, 512, 512,
                                   IMAGEIO_RGB | IMAGEIO_FLOAT, FALSE))
  {
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "could not create the preview pipeline");
  }
  dt_dev_pixelpipe_set_icc(&pipe, DT_COLORSPACE_SRGB, NULL,
                           DT_INTENT_PERCEPTUAL);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf,
                             buf.width, buf.height, buf.iscale);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight,
                                  &pipe.processed_width, &pipe.processed_height);

  dt_dev_pixelpipe_iop_t *target = NULL;
  gboolean upstream_colorout = FALSE;
  for(GList *it = pipe.nodes; it; it = g_list_next(it))
  {
    dt_dev_pixelpipe_iop_t *piece = it->data;
    if(piece->enabled && !strcmp(piece->module->op, "colorout"))
      upstream_colorout = TRUE;
    if(!strcmp(piece->module->op, "magiceraser")
       && piece->module->multi_priority == multi_priority)
    {
      target = piece;
      break;
    }
  }
  const dt_develop_blend_params_t *blend = target ? target->blendop_data : NULL;
  if(!target || !blend || blend->mask_id != mask_id || !upstream_colorout
     || target->buf_in.width <= 0 || target->buf_in.height <= 0
     || target->buf_in.width != target->buf_out.width
     || target->buf_in.height != target->buf_out.height
     || dt_magiceraser_geometry_hash(&dev, target->module->iop_order) != geometry_hash)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "selection geometry changed or the color pipeline is unavailable");
  }
  p->canvas_width = target->buf_in.width;
  p->canvas_height = target->buf_in.height;

  if(pipe.forms)
    g_list_free_full(pipe.forms, (GDestroyNotify)dt_masks_free_form);
  pipe.forms = dt_masks_dup_forms_deep(dev.forms, NULL);
  dt_masks_form_t *group = dt_masks_get_from_id_ext(pipe.forms, mask_id);
  dt_eraser_selection_bounds_t bounds = {
    G_MAXINT64, G_MAXINT64, G_MININT64, G_MININT64, 0, FALSE
  };
  dt_mask_id_t ancestors[DT_ERASER_MAX_MASK_DEPTH];
  if(!group || group->type != DT_MASKS_GROUP
     || !_selection_walk(target->module, target, group, FALSE, 1.0f,
                         ancestors, 0, &bounds, NULL, NULL, NULL)
     || bounds.right <= bounds.left || bounds.bottom <= bounds.top
     || dt_masks_group_hash_ext(DT_INITHASH, group, pipe.forms) != group_hash)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "the selection is unsupported or changed before preview");
  }

  /* Primitive areas bound the rendered mask, including path feather. The
     actual selected pixels tighten this candidate before any image render. */
  const int left = (int)MAX((gint64)0, bounds.left - DT_ERASER_NEARBY_CONTEXT);
  const int top = (int)MAX((gint64)0, bounds.top - DT_ERASER_NEARBY_CONTEXT);
  const int right = (int)MIN((gint64)p->canvas_width,
                             bounds.right + DT_ERASER_NEARBY_CONTEXT);
  const int bottom = (int)MIN((gint64)p->canvas_height,
                              bounds.bottom + DT_ERASER_NEARBY_CONTEXT);
  if(right <= left || bottom <= top
     || right - left > 512 || bottom - top > 512
     || (size_t)(right - left) * (bottom - top) > DT_ERASER_MAX_PREVIEW_PIXELS)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "selection is too large for one full-resolution preview");
  }
  const int candidate_width = right - left;
  const int candidate_height = bottom - top;
  const size_t candidate_pixels = (size_t)candidate_width * candidate_height;
  float *selection = dt_alloc_align_float(candidate_pixels);
  float *holes = bounds.has_difference ? g_try_new0(float, candidate_pixels) : NULL;
  if(!selection || (bounds.has_difference && !holes))
  {
    dt_free_align(selection);
    g_free(holes);
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "out of memory");
  }
  const dt_iop_roi_t roi = { left, top, candidate_width, candidate_height, 1.0f };
  bounds.forms = 0;
  const gboolean leaves_ok = _selection_walk(target->module, target, group,
                                             FALSE, 1.0f, ancestors, 0, &bounds,
                                             &roi, selection, holes);
  memset(selection, 0, candidate_pixels * sizeof(float));
  const gboolean mask_ok = leaves_ok && dt_masks_group_render_roi
    (target->module, target, group, &roi, selection);
  int selected_left = candidate_width, selected_top = candidate_height;
  int selected_right = -1, selected_bottom = -1;
  gboolean mask_valid = mask_ok;
  gboolean hole_overlap = FALSE;
  for(int y = 0; mask_valid && y < candidate_height; y++)
    for(int x = 0; x < candidate_width; x++)
    {
      const float value = selection[(size_t)y * candidate_width + x];
      if(!isfinite(value) || value < 0.0f || value > 1.0f)
      {
        mask_valid = FALSE;
        break;
      }
      /* A missing Difference child can leave the outer group render looking
         successful. Reject that preview instead of filling a declared hole.
         A later union over the hole is conservatively rejected as well. */
      const float hole = holes ? holes[(size_t)y * candidate_width + x] : 0.0f;
      if(hole >= 0.5f && value > 1.0f - hole + 1e-6f)
      {
        mask_valid = FALSE;
        hole_overlap = TRUE;
        break;
      }
      if(value >= 0.5f)
      {
        selected_left = MIN(selected_left, x);
        selected_top = MIN(selected_top, y);
        selected_right = MAX(selected_right, x);
        selected_bottom = MAX(selected_bottom, y);
      }
    }
  if(!mask_valid || selected_right < selected_left)
  {
    dt_free_align(selection);
    g_free(holes);
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, hole_overlap
                            ? "the selection refills a Difference hole"
                            : "the selection contains no fillable area");
  }

  /* The support of the composed mask, not a subtractive path's rectangle,
     sets the ROI. Keep nearby unpainted source pixels for donor selection. */
  p->x = MAX(left, left + selected_left - DT_ERASER_NEARBY_CONTEXT);
  p->y = MAX(top, top + selected_top - DT_ERASER_NEARBY_CONTEXT);
  const int exact_right = (int)MIN((gint64)right,
                                   (gint64)left + selected_right
                                   + DT_ERASER_NEARBY_CONTEXT + 1);
  const int exact_bottom = (int)MIN((gint64)bottom,
                                    (gint64)top + selected_bottom
                                    + DT_ERASER_NEARBY_CONTEXT + 1);
  p->width = exact_right - p->x;
  p->height = exact_bottom - p->y;
  const size_t pixels = (size_t)p->width * p->height;
  if(p->x != left || p->y != top || p->width != candidate_width
     || p->height != candidate_height)
  {
    float *tight = dt_alloc_align_float(pixels);
    if(!tight)
    {
      dt_free_align(selection);
      g_free(holes);
      dt_dev_pixelpipe_cleanup(&pipe);
      dt_mipmap_cache_release(&buf);
      dt_dev_cleanup(&dev);
      return _error(p, error, "out of memory");
    }
    for(int y = 0; y < p->height; y++)
      memcpy(tight + (size_t)y * p->width,
             selection + (size_t)(p->y - top + y) * candidate_width + p->x - left,
             (size_t)p->width * sizeof(float));
    dt_free_align(selection);
    selection = tight;
  }
  p->coverage = g_try_new0(float, pixels);
  p->source_rgb = g_try_new(float, 3 * pixels);
  p->filled_rgb = g_try_new(float, 3 * pixels);
  if(!p->coverage || !p->source_rgb || !p->filled_rgb)
  {
    dt_free_align(selection);
    g_free(holes);
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "out of memory");
  }
  int box_x, box_y, box_w, box_h;
  const gboolean box_ok = dt_inpaint_soft_mask_from_selection
    (selection, p->width, p->height, 8, 8, p->coverage,
     &box_x, &box_y, &box_w, &box_h);
  if(box_ok && holes)
    for(int y = 0; y < p->height; y++)
      for(int x = 0; x < p->width; x++)
      {
        const size_t local = (size_t)y * p->width + x;
        const size_t candidate = (size_t)(p->y - top + y) * candidate_width
                                 + p->x - left + x;
        /* The distance padding can cross a subtractive feather even when
           the composed selection is above its binary threshold. Fade the
           entire hole edge continuously and leave its inner half untouched. */
        if(holes[candidate] > 0.0f)
          p->coverage[local] *= MAX(0.0f, 1.0f - 2.0f * holes[candidate]);
      }
  dt_free_align(selection);
  g_free(holes);
  if(!box_ok)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "the selection contains no fillable area");
  }

  /* Stop at this exact instance. Earlier accepted erasers remain visible in
     the source; later ones and the prospective patch cannot contaminate it. */
  gboolean at_target = FALSE;
  for(GList *it = pipe.nodes; it; it = g_list_next(it))
  {
    dt_dev_pixelpipe_iop_t *piece = it->data;
    if(piece == target) at_target = TRUE;
    if(at_target) piece->enabled = FALSE;
  }
  int rendered_w, rendered_h;
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight,
                                  &rendered_w, &rendered_h);
  const gboolean render_ok = rendered_w == p->canvas_width
    && rendered_h == p->canvas_height
    && !dt_dev_pixelpipe_process_no_gamma(&pipe, &dev,
                                          p->x, p->y, p->width, p->height, 1.0f)
    && pipe.backbuf && pipe.backbuf_width == p->width
    && pipe.backbuf_height == p->height;
  if(!render_ok)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    return _error(p, error, "could not render the source pixels");
  }
  const float *src = (const float *)pipe.backbuf;
  gboolean finite = TRUE;
  for(size_t i = 0; i < pixels && finite; i++)
    for(int c = 0; c < 3; c++)
    {
      const float v = src[4 * i + c];
      if(!isfinite(v)) { finite = FALSE; break; }
      p->source_rgb[3 * i + c] = CLAMPF(v, 0.0f, 1.0f);
    }
  dt_dev_pixelpipe_cleanup(&pipe);
  dt_mipmap_cache_release(&buf);
  dt_dev_cleanup(&dev);
  if(!finite) return _error(p, error, "the source pixels are invalid");

  char source_after[DT_DTDATA_SOURCE_SHA256_LEN] = { 0 };
  if(!dt_dtdata_source_sha256(imgid, source_after)
     || strcmp(p->source_sha256, source_after))
    return _error(p, error, "the source photo changed during preview");
  return p;
}

dt_magiceraser_preview_t *dt_magiceraser_preview_generate(
    const dt_imgid_t imgid, const int multi_priority, const int32_t history_end,
    const dt_mask_id_t mask_id, const dt_hash_t group_hash,
    const dt_hash_t geometry_hash, char **error)
{
  if(error) *error = NULL;
  dt_magiceraser_preview_t *p = _prepare_preview
    (imgid, multi_priority, history_end, mask_id, group_hash, geometry_hash, error);
  if(!p) return NULL;
  if(!_run_nearby_fill(p, error))
    return _error(p, error, "could not make an Experimental Eraser preview");
  return p;
}

// clang-format off
// modelines: These editor modelines have been set by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
