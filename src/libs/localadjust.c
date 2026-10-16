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

#include "common/darktable.h"
#include "common/debug.h"
#include "bauhaus/bauhaus.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "common/undo.h"
#include "control/conf.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

// the local adjustments panel (ARCHITECTURE-lightroom-workflow.md
// section 9): one local adjustment is one localmask instance (the mask
// source) plus one instance of an existing module per non-zero slider
// (the raster sinks). the panel only creates, lists and deletes the
// instances; drawing stays in the mask manager, which edits
// dev->gui_module, so selecting an adjustment hands the focus to its
// localmask instance. blend flags per section 9.1: the source is
// enabled with mask_mode uniformly (no drawn/conditional/raster bit)
// while it has no shapes, each sink is DEVELOP_MASK_ENABLED |
// DEVELOP_MASK_RASTER with blend_mode DEVELOP_BLEND_NORMAL2 and the
// group's multi_priority as raster_mask_instance.

DT_MODULE(1)

typedef enum dt_lib_localadjust_sliders_t
{
  SLIDER_EXPOSURE = 0,
  SLIDER_HIGHLIGHTS,
  SLIDER_SHADOWS,
  SLIDER_WHITES,
  SLIDER_BLACKS,
  SLIDER_TEXTURE,
  SLIDER_CLARITY,
  SLIDER_DEHAZE,
  SLIDER_SATURATION,
  SLIDER_COUNT
} dt_lib_localadjust_sliders_t;

typedef struct dt_lib_localadjust_slider_info_t
{
  const char *label;
  const char *op;     // module providing the sink instance
  const char *field;  // params field, written through introspection
  const float min, max, step;
  const int digits;
  const gboolean percent; // x/100 into the field, as in lightroom_map.c
} dt_lib_localadjust_slider_info_t;

// section 9.2, same scales as the global Lightroom mapping: exposure is
// already EV (identity in the map), the rest are Lightroom percent
static const dt_lib_localadjust_slider_info_t _sliders[SLIDER_COUNT] =
{
  [SLIDER_EXPOSURE]   = { N_("exposure"),   "exposure",         "exposure",
                          -4.0f, 4.0f, 0.01f, 2, FALSE },
  [SLIDER_HIGHLIGHTS] = { N_("highlights"), "toneequal",        "highlights",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_SHADOWS]    = { N_("shadows"),    "toneequal",        "deep_blacks",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_WHITES]     = { N_("whites"),     "toneequal",        "whites",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_BLACKS]     = { N_("blacks"),     "toneequal",        "noise",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_TEXTURE]    = { N_("texture"),    "contrastntexture", "gain_medium",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_CLARITY]    = { N_("clarity"),    "contrastntexture", "gain_coarse",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_DEHAZE]     = { N_("dehaze"),     "hazeremoval",      "strength",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
  [SLIDER_SATURATION] = { N_("saturation"), "colorbalancergb",  "saturation_global",
                          -100.0f, 100.0f, 1.0f, 0, TRUE },
};

typedef struct dt_lib_localadjust_t
{
  GtkWidget *list;
  GtkWidget *add_button, *delete_button;
  GtkWidget *mode_label;
  GtkWidget *sliders_box;
  GtkWidget *slider[SLIDER_COUNT];
  GArray *groups;   // multi_priorities of the localmask instances shown
  gchar *list_signature; // image, owner names, and root/component membership
  dt_imgid_t listed_imgid;
  int selected;     // multi_priority of the selected localmask, -1 = none
  dt_mask_id_t selected_child; // selected component in the chosen parent
  /* set when Confirm created a mask for a shape that is not drawn yet */
  int provisional;
  /* A new-mask or Subtract shape opened an undo group; commit/cancel closes it */
  gboolean undo_hold;
  guint undo_hold_idle;
  gboolean updating;
} dt_lib_localadjust_t;

const char *name(dt_lib_module_t *self)
{
  return _("local adjustments");
}

const char *description(dt_lib_module_t *self)
{
  return _("local adjustments on named masks:\n"
           "one mask shared by exposure, tone and color sliders");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  /* Panels are packed from the highest position, so a larger number
   * sits higher. 15 is directly above the mask manager (10): the
   * effect sliders stay on screen while a component is selected. */
  return 15;
}

static void _update(dt_lib_module_t *self);
static void _refresh_sliders(dt_lib_module_t *self);
static void _show_mode(dt_lib_module_t *self);
static void _row_toggled(GtkToggleButton *button, dt_lib_module_t *self);
static void _child_toggled(GtkToggleButton *button, dt_lib_module_t *self);

static dt_iop_module_t *_find_mask_instance(dt_develop_t *dev, const int priority)
{
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(dt_iop_module_is(mod, "localmask") && mod->multi_priority == priority)
      return mod;
  }
  return NULL;
}

// the sink of one slider in one group: a non-base instance of the
// slider's module whose raster source is the group's localmask
static dt_iop_module_t *_find_sink(dt_develop_t *dev,
                                   const char *op,
                                   const int source_priority)
{
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(mod->multi_priority == 0 || !dt_iop_module_is(mod, op)) continue;
    const dt_develop_blend_params_t *bp = mod->blend_params;
    if(!bp || !(bp->mask_mode & DEVELOP_MASK_RASTER)) continue;
    if(bp->raster_mask_instance != source_priority
       || bp->raster_mask_id != BLEND_RASTER_ID
       || !g_str_equal(bp->raster_mask_source, "localmask"))
      continue;
    return mod;
  }
  return NULL;
}

static float *_field_ptr(dt_iop_module_t *module, const char *name)
{
  if(!module->have_introspection || !module->so->get_introspection) return NULL;
  return dt_introspection_get_child(module->so->get_introspection()->field,
                                    module->params, name, NULL);
}

static float _slider_to_field(const int k, const float v)
{
  return _sliders[k].percent ? v / 100.0f : v;
}

static float _field_to_slider(const int k, const float v)
{
  return _sliders[k].percent ? v * 100.0f : v;
}

static void _end_undo_hold(dt_lib_localadjust_t *d);
static void _abandon_shape(dt_lib_module_t *self, dt_iop_module_t *owner);

static void _sync_source_drawn(dt_iop_module_t *mask)
{
  // a brush stroke stores a shape and a mask id, but the pipe only
  // uses that shape when the drawn bit is set. without it the module
  // blends uniformly and the local exposure hits the whole frame.
  if(!mask || !mask->blend_params || !mask->dev) return;

  dt_develop_blend_params_t bp = *mask->blend_params;
  const gboolean id_ok = dt_is_valid_maskid(bp.mask_id);
  const dt_masks_form_t *grp = id_ok
    ? dt_masks_get_from_id(mask->dev, bp.mask_id) : NULL;
  /* forms may be mid-reload. do not write "no shape" over a link
   * we cannot see yet, or the undo leaves an empty mask beside the brush. */
  if(id_ok && !grp) return;

  const gboolean has_shape = grp && (grp->type & DT_MASKS_GROUP) && grp->points;
  if(!has_shape && !id_ok) return;

  const uint32_t want = has_shape
    ? (DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK)
    : DEVELOP_MASK_ENABLED;
  if(bp.mask_mode == want) return;

  bp.mask_mode = want;
  dt_iop_commit_blend_params(mask, &bp, NULL);
  /* same history row as the shapes, so undo restores the link and the brush together */
  dt_dev_add_masks_history_item(mask->dev, mask, TRUE);
}

static void _commit_sink_blend(dt_iop_module_t *module, const int source_priority)
{
  dt_develop_blend_params_t bp = *module->blend_params;
  bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  bp.blend_mode = DEVELOP_BLEND_NORMAL2;
  g_strlcpy(bp.raster_mask_source, "localmask", sizeof(bp.raster_mask_source));
  bp.raster_mask_instance = source_priority;
  bp.raster_mask_id = BLEND_RASTER_ID;
  bp.raster_mask_invert = FALSE;
  dt_iop_commit_blend_params(module, &bp, NULL);
}

static void _keep_global_module(dt_iop_module_t *sink)
{
  // duplicating a module opens the new instance and, when only one
  // module may be open, closes the global one. the open module on
  // the right is what a whole-photo edit uses, so the masked copy
  // must not stay open in its place.
  if(!sink || !sink->dev) return;

  if(!sink->multi_name_hand_edited)
  {
    g_strlcpy(sink->multi_name, _("local"), sizeof(sink->multi_name));
    sink->multi_name_hand_edited = TRUE;
    if(sink->dev->gui_attached)
      dt_iop_gui_update_header(sink);
  }

  dt_iop_module_t *base = dt_iop_get_module_by_op_priority(sink->dev->iop,
                                                           sink->op, 0);
  if(sink->expanded)
    dt_iop_gui_set_expanded(sink, FALSE, FALSE);
  if(base && base != sink
     && dt_conf_get_bool("darkroom/ui/single_module")
     && !base->expanded)
    dt_iop_gui_set_expanded(base, TRUE, FALSE);
}

/* A drag on one slider is one undo. merge_same_secs (default 10s) skips
 * a new snapshot when the same widget moves again. An empty undo group
 * around that second move would still consume one Undo and do nothing,
 * so the group is only opened when this call writes more than the value. */
static gboolean _same_slider_drag(dt_develop_t *dev, GtkWidget *slider)
{
  if(!dev || !slider || slider != dev->gui_previous_target) return FALSE;
  const float merge = dt_conf_get_float("darkroom/undo/merge_same_secs");
  return dt_get_wtime() < dev->gui_previous_time + merge;
}

static void _set_sink_value(dt_lib_module_t *self, const int k, const float lr_value)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  dt_iop_module_t *mask = _find_mask_instance(dev, d->selected);
  if(!mask) return;

  /* A slider edit is separate from shape creation, including a pending
   * Subtract component on an existing mask. */
  if(d->provisional < 0)
  {
    dt_masks_form_gui_t *gui = dev->form_gui;
    if(gui && gui->creation && gui->creation_module == mask
       && dt_is_valid_maskid(gui->subtract_parentid))
      dt_masks_cancel_creation();
    _end_undo_hold(d);
  }

  GtkWidget *slider = d->slider[k];
  const gboolean same_drag = _same_slider_drag(dev, slider);
  dt_iop_module_t *sink = _find_sink(dev, _sliders[k].op, d->selected);
  const gboolean creating = lr_value != 0.0f && !sink;
  gboolean removing = lr_value == 0.0f && sink;
  for(int j = 0; removing && j < SLIDER_COUNT; j++)
  {
    if(j == k || !g_str_equal(_sliders[j].op, _sliders[k].op)) continue;
    float *value = _field_ptr(sink, _sliders[j].field);
    if(value && *value != 0.0f) removing = FALSE;
  }
  /* Confirm may already be holding one group for the shape being drawn.
   * A nested group stops Undo at the inner marker. */
  const gboolean nest = d->undo_hold
                        || (darktable.undo && darktable.undo->group != DT_UNDO_NONE);
  const gboolean group = !same_drag && !nest && (creating || removing);

  if(group) dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  // the brush may have arrived since the adjustment was created
  _sync_source_drawn(mask);

  if(lr_value == 0.0f && (!sink || removing))
  {
    // remove the instance only when all of its local sliders are zero. during one drag the
    // removal stays in the drag's snapshot instead of a second Undo.
    if(sink)
    {
      if(same_drag) dt_undo_disable_next(darktable.undo);
      dt_iop_gui_delete(sink);
    }
    if(group) dt_undo_end_group(darktable.undo);
    return;
  }

  if(!sink)
  {
    dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop,
                                                             _sliders[k].op, 0);
    if(!base)
    {
      if(group) dt_undo_end_group(darktable.undo);
      return;
    }
    // default params, not the priority-0 blob: an exposure instance
    // reloads at 0 EV with compensation off (exposure.c reload_defaults)
    sink = dt_iop_gui_duplicate(base, FALSE);
    if(!sink)
    {
      if(group) dt_undo_end_group(darktable.undo);
      return;
    }
    _commit_sink_blend(sink, mask->multi_priority);
  }

  float *f = _field_ptr(sink, _sliders[k].field);
  if(!f)
  {
    dt_dev_modulegroups_update_visibility(dev);
    if(group) dt_undo_end_group(darktable.undo);
    return;
  }
  *f = _slider_to_field(k, lr_value);
  // name and fold the masked copy before it is recorded, so the
  // instance on the right reads as local and the global one stays open
  _keep_global_module(sink);
  dt_iop_gui_update(sink);
  _keep_global_module(sink);
  /* blend params are written after duplicate's own visibility refresh */
  dt_dev_modulegroups_update_visibility(dev);
  /* last, so a later drag of this slider merges into this edit */
  dt_dev_add_history_item_target(dev, sink, TRUE, slider);
  if(group) dt_undo_end_group(darktable.undo);
}

static void _slider_changed(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->updating || d->selected < 0) return;

  const int k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "slider-index"));
  // snap to the step so a drag landing near zero really removes the sink
  const float step = _sliders[k].step;
  const float v = roundf(dt_bauhaus_slider_get(w) / step) * step;

  d->updating = TRUE;
  dt_bauhaus_slider_set(w, v);
  _set_sink_value(self, k, v);
  d->updating = FALSE;

  // creating the sink took the module focus; hand it back to the mask
  dt_iop_request_focus(_find_mask_instance(darktable.develop, d->selected));
}

static gboolean _mask_has_shape(const dt_iop_module_t *mask)
{
  if(!mask || !mask->blend_params) return FALSE;
  if(!dt_is_valid_maskid(mask->blend_params->mask_id)) return FALSE;
  const dt_masks_form_t *grp = dt_masks_get_from_id(mask->dev, mask->blend_params->mask_id);
  return grp && (grp->type & DT_MASKS_GROUP) && grp->points;
}

static int _shape_owner_count(dt_develop_t *dev, const dt_mask_id_t id)
{
  int owners = 0;
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(dt_masks_is_in_module(id, mod)) owners++;
  }
  return owners;
}

/* Drop shapes that belong only to this mask. A shape another module
 * also uses is unlinked here and left in place for that module. */
static void _remove_exclusive_shapes(dt_iop_module_t *mask)
{
  if(!mask || !mask->blend_params) return;
  if(!dt_is_valid_maskid(mask->blend_params->mask_id)) return;

  dt_masks_form_t *grp = dt_masks_get_from_id(mask->dev, mask->blend_params->mask_id);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

  // Another module may reference this root group directly or as a child group.
  // Deleting this localmask must leave that group's contents and links intact.
  if(_shape_owner_count(mask->dev, grp->formid) > 1) return;

  GArray *ids = g_array_new(FALSE, FALSE, sizeof(dt_mask_id_t));
  for(GList *p = grp->points; p; p = g_list_next(p))
  {
    const dt_masks_point_group_t *pt = p->data;
    g_array_append_val(ids, pt->formid);
  }

  for(guint i = 0; i < ids->len; i++)
  {
    const dt_mask_id_t id = g_array_index(ids, dt_mask_id_t, i);
    dt_masks_form_t *form = dt_masks_get_from_id(mask->dev, id);
    if(!form || (form->type & DT_MASKS_GROUP)) continue;

    grp = dt_masks_get_from_id(mask->dev, mask->blend_params->mask_id);
    if(!grp) break;
    if(_shape_owner_count(mask->dev, id) <= 1)
      dt_masks_form_remove(mask, NULL, form);
    else
      dt_masks_form_remove(mask, grp, form);
  }
  g_array_free(ids, TRUE);

  if(mask->blend_params
     && dt_is_valid_maskid(mask->blend_params->mask_id))
  {
    grp = dt_masks_get_from_id(mask->dev, mask->blend_params->mask_id);
    if(grp) dt_masks_form_remove(mask, NULL, grp);
  }
}

static void _end_undo_hold(dt_lib_localadjust_t *d)
{
  if(!d) return;
  if(d->undo_hold_idle)
  {
    g_source_remove(d->undo_hold_idle);
    d->undo_hold_idle = 0;
  }
  if(!d->undo_hold) return;
  d->undo_hold = FALSE;
  if(darktable.undo && darktable.undo->group != DT_UNDO_NONE)
    dt_undo_end_group(darktable.undo);
}

/* The brush/ellipse caller still writes the module history item after
 * shape_committed returns. Closing the group on idle keeps that write
 * inside the same Undo. */
static gboolean _end_undo_hold_idle(gpointer user_data)
{
  dt_lib_module_t *self = user_data;
  dt_lib_localadjust_t *d = self->data;
  if(!d) return G_SOURCE_REMOVE;
  d->undo_hold_idle = 0;
  _end_undo_hold(d);
  return G_SOURCE_REMOVE;
}

/* sinks, exclusive shapes, then the mask. the caller owns the undo group. */
static void _destroy_mask(dt_lib_module_t *self, dt_iop_module_t *mask)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(!mask || !dev) return;

  const int priority = mask->multi_priority;
  GList *victims = NULL;
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(mod->multi_priority == 0) continue;
    const dt_develop_blend_params_t *bp = mod->blend_params;
    if(!bp || !(bp->mask_mode & DEVELOP_MASK_RASTER)) continue;
    if(bp->raster_mask_instance != priority
       || !g_str_equal(bp->raster_mask_source, "localmask"))
      continue;
    victims = g_list_prepend(victims, mod);
  }
  for(GList *v = victims; v; v = g_list_next(v))
    dt_iop_gui_delete(v->data);
  g_list_free(victims);

  _remove_exclusive_shapes(mask);
  if(d->provisional == priority) d->provisional = -1;
  if(d->selected == priority) d->selected = -1;
  dt_iop_gui_delete(mask);
  // preserve remaining shapes independently of the removed module
  dt_dev_add_masks_history_item(dev, NULL, FALSE);
}

static dt_iop_module_t *_create_mask(dt_lib_module_t *self, const gboolean grouped)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached) return NULL;

  dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop, "localmask", 0);
  if(!base) return NULL;

  d->updating = TRUE;
  if(grouped) dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  dt_iop_module_t *mask = dt_iop_gui_duplicate(base, FALSE);
  if(mask)
  {
    /* empty until a shape is drawn: sinks fill 0, so the sliders
     * do not change the photo yet */
    dt_develop_blend_params_t bp = *mask->blend_params;
    bp.mask_mode = DEVELOP_MASK_ENABLED;
    bp.mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;
    dt_iop_commit_blend_params(mask, &bp, NULL);
    dt_dev_add_history_item(dev, mask, TRUE);
    dt_iop_gui_update_blending(mask);
    d->selected = mask->multi_priority;
  }
  if(grouped) dt_undo_end_group(darktable.undo);
  d->updating = FALSE;

  _update(self);
  if(mask) dt_iop_request_focus(mask);
  return mask;
}

static gboolean _confirm_new_mask(void)
{
  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  for(GList *wins = gtk_window_list_toplevels();
      wins;
      wins = g_list_delete_link(wins, wins))
  {
    if(gtk_window_is_active(wins->data))
      win = wins->data;
  }

  GtkWidget *dialog = gtk_message_dialog_new
    (win, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
     "%s", _("No mask is selected. A new mask will be created for this shape."));
  gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                         _("Confirm"), GTK_RESPONSE_ACCEPT,
                         _("Cancel"), GTK_RESPONSE_CANCEL,
                         NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_title(GTK_WINDOW(dialog), _("New mask"));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  const int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  return resp == GTK_RESPONSE_ACCEPT;
}

static dt_iop_module_t *_selected_mask(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->selected < 0) return NULL;
  return _find_mask_instance(darktable.develop, d->selected);
}

static void _sync_row_selection(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  GList *items = gtk_container_get_children(GTK_CONTAINER(d->list));
  for(GList *it = items; it; it = g_list_next(it))
  {
    GtkWidget *item = it->data;
    const int priority = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "mask-priority"));
    GtkWidget *parent = g_object_get_data(G_OBJECT(item), "mask-parent");
    GtkWidget *components = g_object_get_data(G_OBJECT(item), "mask-components");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(parent), priority == d->selected);

    GList *children = gtk_container_get_children(GTK_CONTAINER(components));
    for(GList *child = children; child; child = g_list_next(child))
    {
      GtkWidget *row = child->data;
      if(!GTK_IS_TOGGLE_BUTTON(row)) continue;
      const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-formid"));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row),
                                   priority == d->selected && id == d->selected_child);
    }
    g_list_free(children);
  }
  g_list_free(items);
}

static void _clear_overlay(dt_develop_t *dev)
{
  dev->mask_overlay_selected_id = NO_MASKID;
  dev->mask_overlay_selected_group_id = NO_MASKID;
  dev->mask_overlay_selected_state = DT_MASKS_STATE_NONE;
  dev->mask_overlay_selected_owner = NULL;
  dev->mask_overlay_hover_id = NO_MASKID;
  dev->mask_overlay_hover_group_id = NO_MASKID;
  dev->mask_overlay_hover_state = DT_MASKS_STATE_NONE;
  dev->mask_overlay_hover_owner = NULL;
  dev->mask_overlay_imgid = dev->image_storage.id;
  dt_control_queue_redraw_center();
}

static void _select_mask(dt_lib_module_t *self, dt_iop_module_t *owner)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(d->updating) return;

  const int priority = owner && g_list_find(dev->iop, owner)
    && dt_iop_module_is(owner, "localmask")
    && owner->multi_priority > 0
    && _find_mask_instance(dev, owner->multi_priority) == owner
    ? owner->multi_priority : -1;
  if(priority != d->selected) _abandon_shape(self, _selected_mask(self));
  // cleanup can change the row inventory; apply the resolved selection afterwards
  _update(self);

  d->updating = TRUE;
  if(priority != d->selected) d->selected_child = NO_MASKID;
  d->selected = priority;
  _sync_row_selection(self);
  d->updating = FALSE;
  if(priority < 0) _clear_overlay(dev);

  _refresh_sliders(self);
  gtk_widget_set_sensitive(d->sliders_box, priority >= 0);
  gtk_widget_set_sensitive(d->delete_button, priority >= 0);
  _show_mode(self);
  dt_lib_gui_queue_update(self);
}

static gboolean _duplicable_form(dt_develop_t *dev, const dt_masks_form_t *form,
                                  const int depth)
{
  if(!form || depth > 1 || !form->functions
     || !form->functions->duplicate_points)
    return FALSE;

  if(form->type != DT_MASKS_GROUP)
    return form->type == DT_MASKS_BRUSH || form->type == DT_MASKS_ELLIPSE
           || form->type == DT_MASKS_PATH || form->type == DT_MASKS_GRADIENT
           || form->type == DT_MASKS_CIRCLE;

  // Local-adjustment cleanup owns a flat parent with leaf components. Reject
  // nested groups so a failed duplication can remove every new form safely.
  if(depth != 0 || !form->points) return FALSE;
  for(const GList *p = form->points; p; p = g_list_next(p))
  {
    const dt_masks_point_group_t *member = p->data;
    if(!member || !_duplicable_form(dev, dt_masks_get_from_id(dev, member->formid),
                                    depth + 1))
      return FALSE;
  }
  return TRUE;
}

static dt_iop_module_t *_duplicate_mask(dt_lib_module_t *self,
                                        dt_iop_module_t *owner,
                                        const gboolean invert)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !darktable.undo || d->updating || d->undo_hold
     || darktable.undo->group != DT_UNDO_NONE || !owner
     || !g_list_find(dev->iop, owner) || !dt_iop_module_is(owner, "localmask")
     || owner->multi_priority <= 0 || !owner->blend_params || !owner->params
     || (dev->form_gui && dev->form_gui->creation))
    return NULL;

  const dt_masks_form_t *root =
    dt_masks_get_from_id(dev, owner->blend_params->mask_id);
  dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop,
                                                           "localmask", 0);
  if(!root || !(root->type & DT_MASKS_GROUP)
     || !_duplicable_form(dev, root, 0) || !base
     || base->params_size != owner->params_size)
    return NULL;

  // A local adjustment owns every raster sink pointed at its source instance.
  // Validate them all before changing history so none can be silently omitted.
  GPtrArray *sinks = g_ptr_array_new();
  for(const GList *it = dev->iop; it; it = g_list_next(it))
  {
    dt_iop_module_t *old = it->data;
    const dt_develop_blend_params_t *bp = old->blend_params;
    if(!bp || bp->raster_mask_instance != owner->multi_priority
       || !g_str_equal(bp->raster_mask_source, "localmask"))
      continue;

    dt_iop_module_t *sink_base =
      dt_iop_get_module_by_op_priority(dev->iop, old->op, 0);
    if(old->multi_priority == 0
       || !(bp->mask_mode & DEVELOP_MASK_RASTER)
       || bp->raster_mask_id != BLEND_RASTER_ID
       || (bp->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL))
       || !old->params || !sink_base || !sink_base->blend_params
       || sink_base->params_size != old->params_size)
    {
      g_ptr_array_free(sinks, TRUE);
      return NULL;
    }
    g_ptr_array_add(sinks, old);
  }

  d->updating = TRUE;
  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  // Duplicate priority-zero bases: duplicating an active instance adds an
  // unwanted history row for it before the new instance is even created.
  dt_iop_module_t *copy = dt_iop_gui_duplicate(base, FALSE);
  if(!copy) goto failed;

  const dt_mask_id_t copyid =
    dt_masks_form_duplicate(dev, owner->blend_params->mask_id);
  if(!dt_is_valid_maskid(copyid)) goto failed;

  memcpy(copy->params, owner->params, copy->params_size);
  dt_develop_blend_params_t source_bp = *owner->blend_params;
  source_bp.mask_id = copyid;
  if(invert) source_bp.mask_combine ^= DEVELOP_COMBINE_MASKS_POS;
  dt_iop_commit_blend_params(copy, &source_bp, NULL);
  copy->enabled = owner->enabled;
  dt_dev_add_masks_history_item(dev, copy, copy->enabled);
  dt_iop_gui_update_blending(copy);

  for(guint i = 0; i < sinks->len; i++)
  {
    dt_iop_module_t *old = g_ptr_array_index(sinks, i);
    dt_iop_module_t *sink_base =
      dt_iop_get_module_by_op_priority(dev->iop, old->op, 0);
    dt_iop_module_t *new_sink = dt_iop_gui_duplicate(sink_base, FALSE);
    if(!new_sink) goto failed;

    memcpy(new_sink->params, old->params, old->params_size);
    dt_develop_blend_params_t sink_bp = *old->blend_params;
    sink_bp.raster_mask_instance = copy->multi_priority;
    dt_iop_commit_blend_params(new_sink, &sink_bp, NULL);
    new_sink->enabled = old->enabled;
    _keep_global_module(new_sink);
    if(old->multi_name_hand_edited && !g_str_equal(old->multi_name, "local"))
    {
      g_strlcpy(new_sink->multi_name, old->multi_name,
                sizeof(new_sink->multi_name));
      dt_iop_gui_update_header(new_sink);
    }
    dt_iop_gui_update(new_sink);
    dt_dev_add_history_item(dev, new_sink, new_sink->enabled);
  }

  dt_undo_end_group(darktable.undo);
  d->updating = FALSE;
  g_ptr_array_free(sinks, TRUE);
  _select_mask(self, copy);
  dt_iop_request_focus(copy);
  return copy;

failed:
  if(copy) _destroy_mask(self, copy);
  dt_undo_end_group(darktable.undo);
  d->updating = FALSE;
  g_ptr_array_free(sinks, TRUE);
  _update(self);
  return NULL;
}

static dt_iop_module_t *_begin_shape(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  dt_iop_module_t *mask = _selected_mask(self);
  if(d->provisional >= 0 && (!mask || d->provisional != mask->multi_priority))
  {
    _abandon_shape(self, _find_mask_instance(darktable.develop, d->provisional));
    mask = _selected_mask(self);
  }
  if(mask)
  {
    // a tool switch before the first shape keeps the same provisional mask
    dt_iop_request_focus(mask);
    return mask;
  }

  if(!_confirm_new_mask()) return NULL;

  /* the mask and the shape that follows are one edit. the group stays
   * open until the shape is committed or cancelled. */
  if(darktable.undo && darktable.undo->group == DT_UNDO_NONE)
  {
    dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
    d->undo_hold = TRUE;
  }
  mask = _create_mask(self, FALSE);
  if(!mask)
  {
    _end_undo_hold(d);
    return NULL;
  }
  d->provisional = mask->multi_priority;
  return mask;
}

static gboolean _hold_shape_undo(dt_lib_module_t *self,
                                 dt_iop_module_t *owner,
                                 const dt_mask_id_t parentid)
{
  dt_lib_localadjust_t *d = self->data;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  if(!owner || _selected_mask(self) != owner || !gui || !gui->creation
     || gui->creation_module != owner || gui->subtract_parentid != parentid
     || owner->blend_params->mask_id != parentid)
    return FALSE;

  // Finish a prior shape before opening the group for this new component.
  _end_undo_hold(d);
  if(darktable.undo && darktable.undo->group == DT_UNDO_NONE)
  {
    dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
    d->undo_hold = TRUE;
  }
  return TRUE;
}

static void _shape_committed(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  d->provisional = -1;
  _sync_source_drawn(_find_mask_instance(darktable.develop, d->selected));
  _update(self);
  if(d->undo_hold && !d->undo_hold_idle)
    d->undo_hold_idle = g_idle_add(_end_undo_hold_idle, self);
}

static void _shape_cancelled(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->provisional < 0)
  {
    _end_undo_hold(d);
    return;
  }

  dt_iop_module_t *mask = _find_mask_instance(darktable.develop, d->provisional);
  d->provisional = -1;
  gboolean has_adjustment = FALSE;
  for(int k = 0; mask && k < SLIDER_COUNT; k++)
  {
    dt_iop_module_t *sink = _find_sink(darktable.develop, _sliders[k].op, mask->multi_priority);
    float *value = sink ? _field_ptr(sink, _sliders[k].field) : NULL;
    if(value && *value != 0.0f) has_adjustment = TRUE;
  }
  if(mask && !_mask_has_shape(mask) && !has_adjustment)
  {
    const gboolean own_group = !d->undo_hold;
    if(own_group) dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
    _destroy_mask(self, mask);
    if(own_group) dt_undo_end_group(darktable.undo);
    _update(self);
  }
  _end_undo_hold(d);
}

static void _abandon_shape(dt_lib_module_t *self, dt_iop_module_t *owner)
{
  dt_lib_localadjust_t *d = self->data;
  const gboolean updating = d->updating;
  d->updating = TRUE;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  if(owner && gui && gui->creation && gui->creation_module == owner)
    dt_masks_cancel_creation();
  _shape_cancelled(self);
  d->updating = updating;
}

static void _add_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->updating) return;
  _abandon_shape(self, _selected_mask(self));
  _create_mask(self, TRUE);
}

static void _delete_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(d->updating || d->selected < 0 || !dev->gui_attached) return;

  dt_iop_module_t *mask = _find_mask_instance(dev, d->selected);
  if(!mask) return;

  d->updating = TRUE;
  _end_undo_hold(d);
  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  _destroy_mask(self, mask);
  dt_undo_end_group(darktable.undo);
  d->selected = -1;
  d->updating = FALSE;

  _update(self);
}

static void _refresh_sliders(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;

  d->updating = TRUE;
  for(int k = 0; k < SLIDER_COUNT; k++)
  {
    float v = 0.0f;
    if(d->selected >= 0)
    {
      dt_iop_module_t *sink = _find_sink(dev, _sliders[k].op, d->selected);
      if(sink)
      {
        float *f = _field_ptr(sink, _sliders[k].field);
        if(f) v = _field_to_slider(k, *f);
      }
    }
    dt_bauhaus_slider_set(d->slider[k], v);
  }
  d->updating = FALSE;
}

static void _show_mode(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;

  if(d->selected < 0)
  {
    gtk_label_set_markup
      (GTK_LABEL(d->mode_label),
       _("<b>whole photo</b>\n"
         "the modules on the right change the entire picture"));
    return;
  }

  dt_iop_module_t *mask = _find_mask_instance(dev, d->selected);
  gchar *name = mask ? dt_history_item_get_name(mask) : g_strdup(_("mask"));
  gchar *esc = g_markup_escape_text(name, -1);
  const char *empty = _mask_has_shape(mask)
    ? ""
    : _("\nthis mask has no shape yet. the sliders are saved, "
        "and they will not change the photo until a shape is added.");
  gchar *markup = g_strdup_printf
    (_("<b>mask</b> — %s\n"
       "the sliders below change only this mask%s"),
     esc, empty);
  gtk_label_set_markup(GTK_LABEL(d->mode_label), markup);
  g_free(markup);
  g_free(esc);
  g_free(name);
}

static gboolean _has_group(const GArray *groups, const int priority)
{
  for(guint i = 0; i < groups->len; i++)
    if(g_array_index(groups, int, i) == priority) return TRUE;
  return FALSE;
}

static const dt_masks_point_group_t *_component_in_mask(const dt_iop_module_t *mask,
                                                       const dt_mask_id_t id)
{
  if(!mask || !mask->blend_params) return NULL;
  const dt_masks_form_t *root = dt_masks_get_from_id(mask->dev, mask->blend_params->mask_id);
  if(!root || !(root->type & DT_MASKS_GROUP)) return NULL;
  for(const GList *it = root->points; it; it = g_list_next(it))
  {
    const dt_masks_point_group_t *member = it->data;
    if(member && member->formid == id) return member;
  }
  return NULL;
}

static GtkWidget *_named_row(const gchar *name)
{
  GtkWidget *row = gtk_toggle_button_new_with_label(name);
  gtk_widget_set_hexpand(row, TRUE);
  GtkWidget *label = gtk_bin_get_child(GTK_BIN(row));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  return row;
}

static void _disclosure_toggled(GtkToggleButton *button, gpointer user_data)
{
  GtkWidget *components = user_data;
  const gboolean expanded = gtk_toggle_button_get_active(button);
  gtk_button_set_label(GTK_BUTTON(button), expanded ? "▾" : "▸");
  gtk_widget_set_visible(components, expanded);
}

static gboolean _row_enter(GtkWidget *row, GdkEventCrossing *event, gpointer user_data)
{
  (void)event;
  (void)user_data;
  dt_develop_t *dev = darktable.develop;
  const int priority = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-priority"));
  const dt_mask_id_t rootid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-rootid"));
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-formid"));
  dt_iop_module_t *mask = _find_mask_instance(dev, priority);
  if(!mask || !mask->blend_params || mask->blend_params->mask_id != rootid) return FALSE;
  const dt_masks_point_group_t *member = id ? _component_in_mask(mask, id) : NULL;
  if(id && !member) return FALSE;
  dev->mask_overlay_hover_id = id ? id : rootid;
  dev->mask_overlay_hover_group_id = member ? rootid : NO_MASKID;
  dev->mask_overlay_hover_state = member ? member->state : DT_MASKS_STATE_NONE;
  dev->mask_overlay_hover_owner = mask;
  dev->mask_overlay_imgid = dev->image_storage.id;
  dt_control_queue_redraw_center();
  return FALSE;
}

static gboolean _row_leave(GtkWidget *row, GdkEventCrossing *event, gpointer user_data)
{
  (void)event;
  (void)user_data;
  dt_develop_t *dev = darktable.develop;
  const int priority = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-priority"));
  const dt_mask_id_t rootid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-rootid"));
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-formid"));
  if(dev->mask_overlay_hover_owner == _find_mask_instance(dev, priority)
     && dev->mask_overlay_hover_id == (id ? id : rootid))
  {
    dev->mask_overlay_hover_id = NO_MASKID;
    dev->mask_overlay_hover_group_id = NO_MASKID;
    dev->mask_overlay_hover_state = DT_MASKS_STATE_NONE;
    dev->mask_overlay_hover_owner = NULL;
    dt_control_queue_redraw_center();
  }
  return FALSE;
}

typedef struct dt_lib_localadjust_expansion_t
{
  int priority;
  dt_mask_id_t rootid;
  gboolean expanded;
} dt_lib_localadjust_expansion_t;

static void _update(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->updating) return;
  dt_develop_t *dev = darktable.develop;

  // one row per localmask instance beyond the priority-0 one
  GArray *groups = g_array_new(FALSE, FALSE, sizeof(int));
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(dt_iop_module_is(mod, "localmask") && mod->multi_priority > 0)
      g_array_append_val(groups, mod->multi_priority);
  }

  // A priority-only cache misses renamed shapes, added components, and an
  // image change with the same priorities. Keep the exact visible structure.
  GString *signature = g_string_new(NULL);
  g_string_append_printf(signature, "%d;", (int)dev->image_storage.id);
  for(guint i = 0; i < groups->len; i++)
  {
    const int priority = g_array_index(groups, int, i);
    dt_iop_module_t *mask = _find_mask_instance(dev, priority);
    gchar *name = dt_history_item_get_name(mask);
    const dt_mask_id_t rootid = mask->blend_params ? mask->blend_params->mask_id : NO_MASKID;
    const dt_masks_form_t *root = dt_masks_get_from_id(dev, rootid);
    g_string_append_printf(signature, "%d:%d:%zu:", priority, rootid, strlen(name));
    g_string_append(signature, name);
    g_free(name);
    if(root && (root->type & DT_MASKS_GROUP))
      for(const GList *it = root->points; it; it = g_list_next(it))
      {
        const dt_masks_point_group_t *member = it->data;
        if(!member) continue;
        const dt_masks_form_t *form = dt_masks_get_from_id(dev, member->formid);
        const gchar *form_name = form ? form->name : "";
        g_string_append_printf(signature, "/%d:%d:%zu:", member->formid,
                               member->state, strlen(form_name));
        g_string_append(signature, form_name);
      }
    g_string_append_c(signature, ';');
  }

  const gboolean same = g_strcmp0(d->list_signature, signature->str) == 0;

  if(same)
  {
    g_array_free(groups, TRUE);
    g_string_free(signature, TRUE);
  }
  else
  {
    d->updating = TRUE;

    // Preserve disclosure state for the same parent on this image when a
    // component is renamed or added. A different image starts afresh.
    GArray *expanded = g_array_new(FALSE, FALSE, sizeof(dt_lib_localadjust_expansion_t));
    if(d->listed_imgid == dev->image_storage.id)
    {
      GList *old_items = gtk_container_get_children(GTK_CONTAINER(d->list));
      for(GList *it = old_items; it; it = g_list_next(it))
      {
        GtkWidget *item = it->data;
        GtkWidget *disclosure = g_object_get_data(G_OBJECT(item), "mask-disclosure");
        dt_lib_localadjust_expansion_t state = {
          .priority = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "mask-priority")),
          .rootid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "mask-rootid")),
          .expanded = disclosure && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(disclosure))
        };
        g_array_append_val(expanded, state);
      }
      g_list_free(old_items);
    }

    gtk_container_foreach(GTK_CONTAINER(d->list),
                          (GtkCallback)gtk_widget_destroy, NULL);

    if(groups->len == 0)
    {
      d->selected = -1;
      d->selected_child = NO_MASKID;
    }
    else if(!_has_group(groups, d->selected))
    {
      d->selected = g_array_index(groups, int, 0);
      d->selected_child = NO_MASKID;
    }
    if(d->selected_child
       && !_component_in_mask(_find_mask_instance(dev, d->selected), d->selected_child))
      d->selected_child = NO_MASKID;

    for(guint i = 0; i < groups->len; i++)
    {
      const int priority = g_array_index(groups, int, i);
      dt_iop_module_t *mask = _find_mask_instance(dev, priority);
      gchar *item_name = dt_history_item_get_name(mask);
      const dt_mask_id_t rootid = mask->blend_params ? mask->blend_params->mask_id : NO_MASKID;
      const dt_masks_form_t *root = dt_masks_get_from_id(dev, rootid);
      GtkWidget *item = dt_gui_vbox();
      GtkWidget *header = dt_gui_hbox();
      GtkWidget *components = dt_gui_vbox();
      GtkWidget *disclosure = gtk_toggle_button_new_with_label("▸");
      GtkWidget *row = _named_row(item_name);
      gtk_widget_set_tooltip_text(disclosure, _("show or hide this mask's components"));
      gtk_widget_set_tooltip_text
        (row, _("select this mask; the sliders below edit this adjustment"));
      g_free(item_name);
      g_object_set_data(G_OBJECT(item), "mask-priority", GINT_TO_POINTER(priority));
      g_object_set_data(G_OBJECT(item), "mask-rootid", GINT_TO_POINTER(rootid));
      g_object_set_data(G_OBJECT(item), "mask-parent", row);
      g_object_set_data(G_OBJECT(item), "mask-disclosure", disclosure);
      g_object_set_data(G_OBJECT(item), "mask-components", components);
      g_object_set_data(G_OBJECT(row), "mask-priority", GINT_TO_POINTER(priority));
      g_object_set_data(G_OBJECT(row), "mask-rootid", GINT_TO_POINTER(rootid));
      g_signal_connect(G_OBJECT(row), "toggled", G_CALLBACK(_row_toggled), self);
      gtk_widget_add_events(row, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
      g_signal_connect(G_OBJECT(row), "enter-notify-event", G_CALLBACK(_row_enter), self);
      g_signal_connect(G_OBJECT(row), "leave-notify-event", G_CALLBACK(_row_leave), self);
      dt_gui_box_add(header, disclosure, row);
      dt_gui_box_add(item, header, components);
      dt_gui_box_add(d->list, item);

      // A mask without shapes is still an editable adjustment, not a hidden row.
      if(!root || !(root->type & DT_MASKS_GROUP) || !root->points)
      {
        GtkWidget *empty = dt_ui_label_new(_("no components yet"));
        dt_gui_box_add(components, empty);
      }
      else
      {
        for(const GList *it = root->points; it; it = g_list_next(it))
        {
          const dt_masks_point_group_t *member = it->data;
          const dt_masks_form_t *form = member
            ? dt_masks_get_from_id(dev, member->formid) : NULL;
          if(!form) continue;
          GtkWidget *child = _named_row(form->name);
          gtk_widget_set_tooltip_text(child, _("select and edit this mask component"));
          g_object_set_data(G_OBJECT(child), "mask-priority", GINT_TO_POINTER(priority));
          g_object_set_data(G_OBJECT(child), "mask-rootid", GINT_TO_POINTER(rootid));
          g_object_set_data(G_OBJECT(child), "mask-formid", GINT_TO_POINTER(form->formid));
          g_signal_connect(G_OBJECT(child), "toggled", G_CALLBACK(_child_toggled), self);
          gtk_widget_add_events(child, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
          g_signal_connect(G_OBJECT(child), "enter-notify-event", G_CALLBACK(_row_enter), self);
          g_signal_connect(G_OBJECT(child), "leave-notify-event", G_CALLBACK(_row_leave), self);
          dt_gui_box_add(components, child);
        }
      }
      gtk_widget_set_margin_start(components, 20);
      gtk_widget_show_all(components);
      gtk_widget_set_no_show_all(components, TRUE);
      gboolean open = priority == d->selected;
      for(guint n = 0; n < expanded->len; n++)
      {
        const dt_lib_localadjust_expansion_t state =
          g_array_index(expanded, dt_lib_localadjust_expansion_t, n);
        if(state.priority == priority && state.rootid == rootid)
        {
          open = state.expanded;
          break;
        }
      }
      g_signal_connect(G_OBJECT(disclosure), "toggled",
                       G_CALLBACK(_disclosure_toggled), components);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(disclosure), open);
      gtk_widget_set_visible(components, open);
    }
    gtk_widget_show_all(d->list);

    // no-show-all keeps collapsed contents hidden through the show_all above.
    _sync_row_selection(self);

    g_array_free(d->groups, TRUE);
    d->groups = groups;
    g_array_free(expanded, TRUE);
    g_free(d->list_signature);
    d->list_signature = g_string_free(signature, FALSE);
    d->listed_imgid = dev->image_storage.id;
    d->updating = FALSE;
  }

  _refresh_sliders(self);
  gtk_widget_set_sensitive(d->sliders_box, d->selected >= 0);
  gtk_widget_set_sensitive(d->delete_button, d->selected >= 0);
  _show_mode(self);
}

static void _row_toggled(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->updating) return;

  const int priority = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mask-priority"));

  // Clicking the parent after a child returns to the whole mask. Otherwise
  // one adjustment stays chosen when its highlighted parent is clicked again.
  if(!gtk_toggle_button_get_active(button))
  {
    if(priority != d->selected) return;
    if(d->selected_child == NO_MASKID)
    {
      d->updating = TRUE;
      gtk_toggle_button_set_active(button, TRUE);
      d->updating = FALSE;
      return;
    }
  }

  if(priority != d->selected) _abandon_shape(self, _selected_mask(self));
  d->updating = TRUE;
  d->selected = priority;
  d->selected_child = NO_MASKID;
  _sync_row_selection(self);
  d->updating = FALSE;

  _refresh_sliders(self);
  gtk_widget_set_sensitive(d->sliders_box, d->selected >= 0);
  gtk_widget_set_sensitive(d->delete_button, d->selected >= 0);
  _show_mode(self);

  dt_iop_module_t *mask = _find_mask_instance(darktable.develop, d->selected);
  dt_iop_request_focus(mask);
  if(_mask_has_shape(mask))
  {
    dt_masks_set_edit_mode(mask, DT_MASKS_EDIT_FULL);
    dt_develop_t *dev = darktable.develop;
    dev->mask_overlay_selected_id = mask->blend_params->mask_id;
    dev->mask_overlay_selected_group_id = NO_MASKID;
    dev->mask_overlay_selected_state = DT_MASKS_STATE_NONE;
    dev->mask_overlay_selected_owner = mask;
    dev->mask_overlay_imgid = dev->image_storage.id;
    dt_control_queue_redraw_center();
  }
  else
    _clear_overlay(darktable.develop);
  // cleanup can remove a row; rebuild only after this callback releases it
  dt_lib_gui_queue_update(self);
}

static void _child_toggled(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->updating) return;
  const int priority = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mask-priority"));
  const dt_mask_id_t rootid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mask-rootid"));
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mask-formid"));
  if(!gtk_toggle_button_get_active(button))
  {
    if(priority == d->selected && id == d->selected_child)
    {
      d->updating = TRUE;
      gtk_toggle_button_set_active(button, TRUE);
      d->updating = FALSE;
    }
    return;
  }

  dt_develop_t *dev = darktable.develop;
  dt_iop_module_t *mask = _find_mask_instance(dev, priority);
  if(!mask || !mask->blend_params || mask->blend_params->mask_id != rootid)
  {
    dt_lib_gui_queue_update(self);
    return;
  }
  const dt_masks_point_group_t *member = _component_in_mask(mask, id);
  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  if(!member || !form)
  {
    dt_lib_gui_queue_update(self);
    return;
  }
  const uint32_t state = member->state;
  if(priority != d->selected) _abandon_shape(self, _selected_mask(self));
  d->updating = TRUE;
  d->selected = priority;
  d->selected_child = id;
  _sync_row_selection(self);
  d->updating = FALSE;

  _refresh_sliders(self);
  gtk_widget_set_sensitive(d->sliders_box, TRUE);
  gtk_widget_set_sensitive(d->delete_button, TRUE);
  _show_mode(self);

  // Select the canvas form directly: Mask Manager may hide localmask rows.
  dt_iop_request_focus(mask);
  dt_masks_set_edit_mode_single_form(mask, id, DT_MASKS_EDIT_FULL);
  dt_masks_select_form(mask, form);
  dev->mask_overlay_selected_id = id;
  dev->mask_overlay_selected_group_id = rootid;
  dev->mask_overlay_selected_state = state;
  dev->mask_overlay_selected_owner = mask;
  dev->mask_overlay_imgid = dev->image_storage.id;
  dt_control_queue_redraw_center();
  dt_lib_gui_queue_update(self);
}

void gui_update(dt_lib_module_t *self)
{
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    _update(self);
}

static void _signal_callback(gpointer instance, dt_lib_module_t *self)
{
  // Image initialization signals can precede history replay. Refresh on
  // the next draw so the panel reads the completed live module state.
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_lib_gui_queue_update(self);
}

static void _image_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  /* a shape group must not stay open across photos */
  dt_lib_localadjust_t *d = self->data;
  _end_undo_hold(d);
  d->provisional = -1;
  d->selected_child = NO_MASKID;
  _clear_overlay(darktable.develop);
  _signal_callback(instance, self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = calloc(1, sizeof(dt_lib_localadjust_t));

  self->data = (void *)d;
  d->selected = -1;
  d->selected_child = NO_MASKID;
  d->listed_imgid = -1;
  d->provisional = -1;
  d->groups = g_array_new(FALSE, FALSE, sizeof(int));

  d->add_button = gtk_button_new_with_label(_("new adjustment"));
  gtk_widget_set_tooltip_text(d->add_button,
                              _("add a local adjustment: one mask, "
                                "shared by the local sliders"));
  g_signal_connect(G_OBJECT(d->add_button), "clicked",
                   G_CALLBACK(_add_clicked), self);

  d->delete_button = gtk_button_new_with_label(_("delete"));
  gtk_widget_set_tooltip_text(d->delete_button,
                              _("delete the selected adjustment: its mask "
                                "and all its local slider instances"));
  g_signal_connect(G_OBJECT(d->delete_button), "clicked",
                   G_CALLBACK(_delete_clicked), self);

  d->list = dt_gui_vbox();

  d->mode_label = gtk_label_new(NULL);
  gtk_label_set_line_wrap(GTK_LABEL(d->mode_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(d->mode_label), 0.0f);
  gtk_widget_set_margin_top(d->mode_label, 6);
  gtk_widget_set_margin_bottom(d->mode_label, 4);
  gtk_widget_set_tooltip_text
    (d->mode_label,
     _("a selected mask is edited with the sliders in this panel.\n"
       "the modules on the right always edit the whole photo.\n"
       "a module named \"local\" belongs to the selected mask."));

  d->sliders_box = dt_gui_vbox();
  for(int k = 0; k < SLIDER_COUNT; k++)
  {
    GtkWidget *s = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                                _sliders[k].min,
                                                _sliders[k].max,
                                                _sliders[k].step, 0.0f,
                                                _sliders[k].digits);
    dt_bauhaus_widget_set_label(s, NULL, _sliders[k].label);
    g_object_set_data(G_OBJECT(s), "slider-index", GINT_TO_POINTER(k));
    g_signal_connect(G_OBJECT(s), "value-changed",
                     G_CALLBACK(_slider_changed), self);
    d->slider[k] = s;
    dt_gui_box_add(d->sliders_box, s);
  }

  self->widget = dt_gui_vbox(dt_gui_hbox(d->add_button, d->delete_button),
                             d->list, d->mode_label, d->sliders_box);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _image_changed_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _signal_callback);

  darktable.develop->proxy.localadjust.module = self;
  darktable.develop->proxy.localadjust.selected_mask = _selected_mask;
  darktable.develop->proxy.localadjust.select_mask = _select_mask;
  darktable.develop->proxy.localadjust.duplicate_mask = _duplicate_mask;
  darktable.develop->proxy.localadjust.begin_shape = _begin_shape;
  darktable.develop->proxy.localadjust.hold_shape_undo = _hold_shape_undo;
  darktable.develop->proxy.localadjust.shape_committed = _shape_committed;
  darktable.develop->proxy.localadjust.shape_cancelled = _shape_cancelled;

  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  _end_undo_hold(d);
  if(darktable.develop->proxy.localadjust.module == self)
  {
    darktable.develop->proxy.localadjust.module = NULL;
    darktable.develop->proxy.localadjust.selected_mask = NULL;
    darktable.develop->proxy.localadjust.select_mask = NULL;
    darktable.develop->proxy.localadjust.duplicate_mask = NULL;
    darktable.develop->proxy.localadjust.begin_shape = NULL;
    darktable.develop->proxy.localadjust.hold_shape_undo = NULL;
    darktable.develop->proxy.localadjust.shape_committed = NULL;
    darktable.develop->proxy.localadjust.shape_cancelled = NULL;
  }
  g_array_free(d->groups, TRUE);
  g_free(d->list_signature);
  free(d);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
