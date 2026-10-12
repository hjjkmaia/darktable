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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// a named mask source for local adjustments: the module copies pixels
// through unchanged and exists only so that one instance owns one
// drawn mask, rasterized by the blending framework and shared by the
// local adjustment instances downstream. It has no power button and
// no sliders; the local adjustments panel creates, enables and
// deletes instances.

// the blend flags the panel must write on the instances of a group:
// - no shapes yet: this instance enabled, mask_mode without
//   DEVELOP_MASK_MASK, DEVELOP_MASK_CONDITIONAL or DEVELOP_MASK_RASTER,
//   so every raster sink fills with 0
// - shapes present: mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
//   mask_combine = DEVELOP_COMBINE_NORM_EXCL, DEVELOP_COMBINE_MASKS_POS
//   stays clear; opacity on this instance is the mask amount
// - each sink instance: mask_mode = DEVELOP_MASK_ENABLED |
//   DEVELOP_MASK_RASTER, blend_mode = DEVELOP_BLEND_NORMAL2,
//   raster_mask_source = "localmask", raster_mask_instance = this
//   instance's multi_priority, raster_mask_id = BLEND_RASTER_ID,
//   raster_mask_invert = FALSE

DT_MODULE_INTROSPECTION(1, dt_iop_localmask_params_t)

typedef struct dt_iop_localmask_params_t
{
  // no adjustable parameters in v1; the field keeps the params blob
  // non-empty for introspection
  int reserved; // $DEFAULT: 0
} dt_iop_localmask_params_t;

typedef struct dt_iop_localmask_gui_data_t
{
  GtkWidget *label;
} dt_iop_localmask_gui_data_t;

const char *name()
{
  return _("mask");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("mask source shared by the instances of a local adjustment"),
     _("creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  // blending-capable so the drawn mask is rasterized and advertised
  // as a raster source; not a raster-mode module itself
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  // identity: the module only owns the mask, the pixel work happens
  // in the local adjustment instances downstream
  dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out);
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  // default_enabled stays FALSE on purpose: with hide_enable_button
  // set, default_enabled would force the module on for every image
  // (develop.c). instead the local adjustments panel enables the
  // instance for the life of its adjustment
  self->hide_enable_button = TRUE;
}

void gui_init(dt_iop_module_t *self)
{
  // Normal GUI updates refresh blending only for modules with GUI data.
  dt_iop_localmask_gui_data_t *g = IOP_GUI_ALLOC(localmask);
  GtkWidget *label = g->label = dt_ui_label_new(_("managed by the local adjustments panel"));
  gtk_widget_set_tooltip_text
    (label,
     _("this instance owns one mask shared by the local adjustment's sliders\n"
       "create and delete local adjustments in the local adjustments panel"));
  self->widget = dt_gui_vbox(label);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
