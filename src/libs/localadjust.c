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
#include "gui/gtk.h"
#include "libs/lib.h"

#include <gtk/gtk.h>
#include <stdlib.h>

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
  GtkWidget *sliders_box;
  GtkWidget *slider[SLIDER_COUNT];
  GArray *groups;   // multi_priorities of the localmask instances shown
  int selected;     // multi_priority of the selected localmask, -1 = none
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
  return 20;
}

static void _update(dt_lib_module_t *self);

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

static void _set_sink_value(dt_lib_module_t *self, const int k, const float lr_value)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  dt_iop_module_t *mask = _find_mask_instance(dev, d->selected);
  if(!mask) return;

  dt_iop_module_t *sink = _find_sink(dev, _sliders[k].op, d->selected);
  if(lr_value == 0.0f)
  {
    // a zero local slider removes that instance
    if(sink) dt_iop_gui_delete(sink);
    return;
  }

  if(!sink)
  {
    dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop,
                                                             _sliders[k].op, 0);
    if(!base) return;
    // default params, not the priority-0 blob: an exposure instance
    // reloads at 0 EV with compensation off (exposure.c reload_defaults)
    sink = dt_iop_gui_duplicate(base, FALSE);
    if(!sink) return;
    _commit_sink_blend(sink, mask->multi_priority);
  }

  float *f = _field_ptr(sink, _sliders[k].field);
  if(!f) return;
  *f = _slider_to_field(k, lr_value);
  // records the value and keeps the instance enabled
  dt_dev_add_history_item(dev, sink, TRUE);
  dt_iop_gui_update(sink);
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

static void _add_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(!dev->gui_attached) return;

  dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop, "localmask", 0);
  if(!base) return;

  d->updating = TRUE;
  dt_iop_module_t *mask = dt_iop_gui_duplicate(base, FALSE);
  if(mask)
  {
    // the empty state: enabled, mask_mode uniformly, none of the drawn,
    // conditional or raster bits, so the sinks fill 0 until a shape exists
    dt_develop_blend_params_t bp = *mask->blend_params;
    bp.mask_mode = DEVELOP_MASK_ENABLED;
    dt_iop_commit_blend_params(mask, &bp, NULL);
    // the instance stays enabled for the life of the group
    dt_dev_add_history_item(dev, mask, TRUE);
    d->selected = mask->multi_priority;
  }
  d->updating = FALSE;

  _update(self);
  // the mask manager edits dev->gui_module, so the brush applies here
  if(mask) dt_iop_request_focus(mask);
}

static void _delete_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(d->updating || d->selected < 0 || !dev->gui_attached) return;

  dt_iop_module_t *mask = _find_mask_instance(dev, d->selected);
  if(!mask) return;

  d->updating = TRUE;
  // collect before deleting: dt_iop_gui_delete() frees the module
  GList *victims = NULL;
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(mod->multi_priority == 0) continue;
    const dt_develop_blend_params_t *bp = mod->blend_params;
    if(!bp || !(bp->mask_mode & DEVELOP_MASK_RASTER)) continue;
    if(bp->raster_mask_instance != d->selected
       || !g_str_equal(bp->raster_mask_source, "localmask"))
      continue;
    victims = g_list_prepend(victims, mod);
  }
  for(GList *v = victims; v; v = g_list_next(v))
    dt_iop_gui_delete(v->data);
  g_list_free(victims);
  dt_iop_gui_delete(mask);
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

static gboolean _has_group(const GArray *groups, const int priority)
{
  for(guint i = 0; i < groups->len; i++)
    if(g_array_index(groups, int, i) == priority) return TRUE;
  return FALSE;
}

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

  const gboolean same = groups->len == d->groups->len
    && (groups->len == 0
        || !memcmp(groups->data, d->groups->data, groups->len * sizeof(int)));

  if(same)
  {
    g_array_free(groups, TRUE);
  }
  else
  {
    d->updating = TRUE;

    gtk_container_foreach(GTK_CONTAINER(d->list),
                          (GtkCallback)gtk_widget_destroy, NULL);

    if(groups->len == 0)
      d->selected = -1;
    else if(!_has_group(groups, d->selected))
      d->selected = g_array_index(groups, int, 0);

    GtkListBoxRow *select_row = NULL;
    for(guint i = 0; i < groups->len; i++)
    {
      const int priority = g_array_index(groups, int, i);
      dt_iop_module_t *mask = _find_mask_instance(dev, priority);
      gchar *item_name = dt_history_item_get_name(mask);
      GtkWidget *row = gtk_list_box_row_new();
      GtkWidget *label = dt_ui_label_new(item_name);
      gtk_widget_set_tooltip_text
        (label, _("one local adjustment: draw on this mask with the "
                  "mask manager, adjust it with the sliders below"));
      g_free(item_name);
      gtk_container_add(GTK_CONTAINER(row), label);
      g_object_set_data(G_OBJECT(row), "mask-priority", GINT_TO_POINTER(priority));
      gtk_container_add(GTK_CONTAINER(d->list), row);
      if(priority == d->selected) select_row = GTK_LIST_BOX_ROW(row);
    }
    gtk_widget_show_all(d->list);

    if(select_row)
      gtk_list_box_select_row(GTK_LIST_BOX(d->list), select_row);
    else
      gtk_list_box_unselect_all(GTK_LIST_BOX(d->list));

    g_array_free(d->groups, TRUE);
    d->groups = groups;
    d->updating = FALSE;
  }

  _refresh_sliders(self);
  gtk_widget_set_sensitive(d->sliders_box, d->selected >= 0);
  gtk_widget_set_sensitive(d->delete_button, d->selected >= 0);
}

static void _row_selected(GtkListBox *box, GtkListBoxRow *row, dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  if(d->updating) return;

  d->selected = row
    ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mask-priority"))
    : -1;
  _refresh_sliders(self);
  gtk_widget_set_sensitive(d->sliders_box, d->selected >= 0);
  gtk_widget_set_sensitive(d->delete_button, d->selected >= 0);

  // the existing mask manager edits this mask through the module focus
  dt_iop_request_focus(_find_mask_instance(darktable.develop, d->selected));
}

static void _signal_callback(gpointer instance, dt_lib_module_t *self)
{
  // only in darkroom, so let's avoid any update when in lighttable
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    _update(self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = calloc(1, sizeof(dt_lib_localadjust_t));

  self->data = (void *)d;
  d->selected = -1;
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

  d->list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(d->list), GTK_SELECTION_SINGLE);
  g_signal_connect(G_OBJECT(d->list), "row-selected",
                   G_CALLBACK(_row_selected), self);

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
                             d->list, d->sliders_box);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _signal_callback);

  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_localadjust_t *d = self->data;
  g_array_free(d->groups, TRUE);
  free(d);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
