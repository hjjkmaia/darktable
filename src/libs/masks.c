/*
    This file is part of darktable,
    Copyright (C) 2013-2026 darktable developers.

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

#include "common/gdk_event_utils.h"

#include "develop/masks.h"
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#pragma GCC diagnostic ignored "-Wshadow"

static void _lib_masks_recreate_list(dt_lib_module_t *self);
static void _lib_masks_update_list(dt_lib_module_t *self);
static void _lib_masks_selection_change(dt_lib_module_t *self,
                                        struct dt_iop_module_t *module,
                                        const dt_mask_id_t selectid);
static gboolean _lib_masks_selection_change_r(GtkTreeModel *model,
                                              GtkTreeSelection *selection,
                                              GtkTreeIter *iter,
                                              struct dt_iop_module_t *module,
                                              const dt_mask_id_t selectid,
                                              const int level);
static void _lib_masks_get_values(GtkTreeModel *model,
                                  GtkTreeIter *iter,
                                  dt_iop_module_t **module,
                                  dt_mask_id_t *groupid,
                                  dt_mask_id_t *formid);
static dt_iop_module_t *_selected_group_owner(GtkTreeModel *model,
                                               const GList *items,
                                               dt_mask_id_t *groupid);
static dt_iop_module_t *_selected_localmask_group_owner(GtkTreeModel *model,
                                                         const GList *items,
                                                         dt_mask_id_t *groupid);
static void _overlay_set_selected(GtkTreeSelection *selection);
static gboolean
_update_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data);

typedef struct dt_lib_masks_t
{
  /* vbox with managed history items */
  GtkWidget *bt_circle, *bt_path, *bt_gradient, *bt_ellipse, *bt_brush;
#ifdef HAVE_AI
  GtkWidget *bt_object;
#endif
  GtkWidget *treeview, *show_overlay, *all_shapes;
  dt_gui_collapsible_section_t cs;
  GtkWidget *property[DT_MASKS_PROPERTY_LAST];
  GtkWidget *pressure, *smoothing;
  /* shown only while the brush is armed */
  GtkWidget *brush_box, *brush_size, *brush_feather, *brush_flow, *brush_density, *brush_erase;
  gboolean brush_updating;
  float last_value[DT_MASKS_PROPERTY_LAST];
  GtkWidget *none_label;
  // path-only shrink/grow (outset/inset) control. The slider is a signed offset
  // measured from the shape's baseline (captured by the path mask the first time
  // it is resized); 0 restores it. The unit (px / %) is a toggle in the slider's
  // quad. The baseline and the per-offset result cache live in the path mask
  // code, so the slider just sets and mirrors the current offset.
  GtkWidget *resize_box, *resize_amount;
  guint resize_timer;       // debounce source id (0 = none)
  gboolean resize_updating; // guard: programmatic slider change, don't commit

  GdkPixbuf *ic_inverse, *ic_union, *ic_intersection;
  GdkPixbuf *ic_difference, *ic_sum, *ic_exclusion, *ic_used;

  // a selection requested (e.g. right after creating a shape) before the tree
  // had the matching row: re-applied once gui_update rebuilds the tree. 0 = none.
  dt_mask_id_t pending_selectid;
  struct dt_iop_module_t *pending_selmodule;

  // structure hash of the tree as last built; when unchanged (e.g. a slider edit
  // that only alters shape parameters) gui_update refreshes rows in place instead
  // of recreating the store, so the panel scroll never moves. See _forms_structure_hash.
  guint tree_hash;
  gboolean tree_hash_valid;

  dt_mask_id_t copy_drag_groupid, copy_drag_formid;
  double copy_drag_start_x, copy_drag_start_y;

} dt_lib_masks_t;

static void _resize_update(dt_lib_masks_t *d);

const char *name(dt_lib_module_t *self)
{
  return _("mask manager (advanced)");
}

const char *description(dt_lib_module_t *self)
{
  return _("advanced inventory of drawn shapes and module mask groups");
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
  /* Keep the advanced mask tools directly below Local Adjustments. */
  return 1050;
}

static gboolean _selected_masks_are_used(GtkTreeModel *model,
                                         GList *selected,
                                         dt_iop_module_t *module)
{
  for(GList *item = selected; item; item = g_list_next(item))
  {
    GtkTreePath *ipath = (GtkTreePath *)item->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, ipath))
    {
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, NULL, NULL, &id);

      if(dt_is_valid_maskid(id)
         && dt_masks_is_in_module(id, module))
      {
        return TRUE;
      }
    }
  }

  return FALSE;
}

void expanded_state(dt_lib_module_t *self,
                    const gboolean expanded)
{
  // if not expanded we may want to disable the mask

  if (!expanded)
  {
    dt_develop_t *dev = darktable.develop;
    dt_iop_module_t *mod = dev->gui_module;
    const dt_iop_gui_blend_data_t *bd = mod ? mod->blend_data : NULL;

    /*
      We hide the masks if:
      - If the active module is not enabled
      - If the active module is enabled but has no mask
      - There is some selected masks in mask manage
      - If the active module is enabled but the selected masks are not used by the
        module.
    */

    dt_lib_masks_t *lm = self->data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    GtkTreeModel *model = NULL;
    GList *selected = gtk_tree_selection_get_selected_rows(selection, &model);

    if (!(mod
          && mod->enabled
          && (mod->blend_params->mask_mode & DEVELOP_MASK_MASK)
          && bd->masks_shown != DT_MASKS_EDIT_OFF
          && selected
          && _selected_masks_are_used(model, selected, mod)))
    {
      dt_masks_change_form_gui(NULL);
      dt_control_queue_redraw_center();
    }
  }
  else
  {
    // when the mask manager is opened, reflect the shape currently selected on
    // the canvas so its controls show right away, instead of the user having to
    // click the shape in the tree first.
    dt_develop_t *dev = darktable.develop;
    if(dt_is_valid_maskid(dev->mask_form_selected_id))
      _lib_masks_selection_change(self, dev->gui_module, dev->mask_form_selected_id);
  }
}

typedef enum dt_masks_tree_cols_t
{
  TREE_TEXT = 0,
  TREE_MODULE,
  TREE_GROUPID,
  TREE_FORMID,
  TREE_EDITABLE,
  TREE_IC_OP,
  TREE_IC_OP_VISIBLE,
  TREE_IC_INVERSE,
  TREE_IC_INVERSE_VISIBLE,
  TREE_IC_USED,
  TREE_IC_USED_VISIBLE,
  TREE_USED_TEXT,
  TREE_COUNT
} dt_masks_tree_cols_t;

// boolean = TRUE renders as a checkbox; min/max/relative are unused
const struct
{
  gchar *name;
  gchar *format;
  float min, max;
  gboolean relative;
  gboolean boolean;
} _masks_properties[DT_MASKS_PROPERTY_LAST]
  = { [ DT_MASKS_PROPERTY_OPACITY] = {N_("opacity"), "%", 0, 1, FALSE, FALSE },
      [ DT_MASKS_PROPERTY_SIZE] = { N_("size"), "%", 0.0001, 1, TRUE, FALSE },
      [ DT_MASKS_PROPERTY_HARDNESS] = { N_("hardness"), "%", 0.0001, 1, TRUE, FALSE },
      [ DT_MASKS_PROPERTY_FEATHER] = { N_("feather"), "%", 0.0001, 1, TRUE, FALSE },
      [ DT_MASKS_PROPERTY_ROTATION] = { N_("rotation"), "°", 0, 360, FALSE, FALSE },
      [ DT_MASKS_PROPERTY_CURVATURE] = { N_("curvature"), "%", -1, 1, FALSE, FALSE },
      [ DT_MASKS_PROPERTY_COMPRESSION] = { N_("compression"), "%", 0.0001, 1, TRUE, FALSE },
      [ DT_MASKS_PROPERTY_CLEANUP] = { N_("cleanup"), "", 0, 100, FALSE, FALSE },
      [ DT_MASKS_PROPERTY_SMOOTHING] = { N_("smoothing"), "", 0, 1.3, FALSE, FALSE },
      [ DT_MASKS_PROPERTY_REFINE] = { N_("refine mask boundary"), "", 0, 1, FALSE, TRUE },
};

gboolean _timeout_show_all_feathers(gpointer userdata)
{
  dt_masks_form_gui_t *gui = userdata;
  gui->show_all_feathers = 0;
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

static void _property_changed(GtkWidget *widget, dt_masks_property_t prop)
{
  dt_lib_module_t *self = darktable.develop->proxy.masks.module;
  dt_lib_masks_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(!form || !gui)
  {
    gtk_widget_hide(widget);
    return;
  }

  const gboolean is_bool = _masks_properties[prop].boolean;
  const float value = is_bool
    ? (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))
    : dt_bauhaus_slider_get(widget);

  // a size/feather/rotation edit reshapes the path and drops its shrink/grow
  // baseline (see path.c); remember that so we can reset the slider to 0 below.
  const gboolean reshaped = (value != d->last_value[prop]) &&
                            (prop == DT_MASKS_PROPERTY_SIZE || prop == DT_MASKS_PROPERTY_FEATHER ||
                             prop == DT_MASKS_PROPERTY_ROTATION);

  DT_ENTER_GUI_UPDATE();
  int count = 0, pos = 0;
  float sum = 0, min = _masks_properties[prop].min, max = _masks_properties[prop].max;
  if(!is_bool)
  {
    if(_masks_properties[prop].relative)
    {
      max /= min;
      min /= _masks_properties[prop].max;
    }
    else
    {
      max -= min;
      min -= _masks_properties[prop].max;
    }
  }

  if(prop == DT_MASKS_PROPERTY_OPACITY && gui->creation)
  {
    float opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    opacity = CLAMP(opacity + value - d->last_value[prop], 0.05f, 1.0f);
    dt_conf_set_float("plugins/darkroom/masks/opacity", opacity);
    sum += opacity;
    ++count;
  }
  else if(!(form->type & DT_MASKS_GROUP)
          && form->functions
          && form->functions->modify_property)
  {
    form->functions->modify_property(form, prop, d->last_value[prop],
                                     value, &sum, &count, &min, &max);
    if(!gui->creation && value != d->last_value[prop])
      dt_masks_gui_form_create(form, gui, pos, dev->gui_module);
  }
  else
  {
    for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts), pos++)
    {
      dt_masks_point_group_t *fpt = fpts->data;
      dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
      if(!sel
         || (dev->mask_form_selected_id && dev->mask_form_selected_id != sel->formid))
        continue;;

      if(prop == DT_MASKS_PROPERTY_OPACITY && dt_is_valid_maskid(fpt->parentid))
      {
        const float new_opacity = dt_masks_form_change_opacity(sel, fpt->parentid,
                                                         value - d->last_value[prop]);
        sum += new_opacity;
        max = fminf(max, 1.0f - new_opacity);
        min = fmaxf(min, .05f - new_opacity);
        ++count;
      }
      else
      {
        const int saved_count = count;

        if(sel->functions && sel->functions->modify_property)
          sel->functions->modify_property(sel, prop, d->last_value[prop],
                                          value, &sum, &count, &min, &max);

        if(count != saved_count
           && value != d->last_value[prop])
        {
          // we recreate the form points
          dt_masks_gui_form_create(sel, gui, pos, dev->gui_module);
        }
      }
    }
  }

  gtk_widget_set_visible(widget, count != 0);
  if(count)
  {
    if(value != d->last_value[prop]
       && sum / count != d->last_value[prop]
       && prop != DT_MASKS_PROPERTY_OPACITY
       && !gui->creation)
    {
      if(gui->show_all_feathers)
        g_source_remove(gui->show_all_feathers);

      gui->show_all_feathers = g_timeout_add_seconds(2, _timeout_show_all_feathers, gui);

      // we save the new parameters
      dt_dev_add_masks_history_item(darktable.develop, dev->gui_module, TRUE);
    }

    if(is_bool)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
                                   (sum / count) > 0.5f);
      d->last_value[prop] =
        (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    }
    else
    {
      if(_masks_properties[prop].relative)
      {
        max *= sum / count;
        min *= sum / count;
      }
      else
      {
        max += sum / count;
        min += sum / count;
      }

      if(dt_isnan(min)) min = _masks_properties[prop].min;
      if(dt_isnan(max)) max = _masks_properties[prop].max;
      dt_bauhaus_slider_set_soft_range(widget, min, max);

      dt_bauhaus_slider_set(widget, sum / count);
      d->last_value[prop] = dt_bauhaus_slider_get(widget);
    }

    gtk_widget_hide(d->none_label);
    dt_control_queue_redraw_center();
  }

  DT_LEAVE_GUI_UPDATE();

  // the recreate-list triggered above is skipped while we hold the gui-update
  // guard, so refresh the shrink/grow slider here: a reshape reset the path's
  // baseline, so this reads back 0.
  if(reshaped)
    _resize_update(d);
}

// Quad for the shrink/grow slider's unit toggle: always shows "%" inside a
// button-like square frame. Its active state (drawn brighter by bauhaus) tells
// whether % mode is engaged; the slider label spells out the unit. Rendered
// with the shared bauhaus label font; the caller has set the source color.
static void _paint_resize_unit(
  cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags, void *data)
{
  const char *txt = "%";
  cairo_save(cr);

  // square frame fitting the quad area, stroke kept inside via a half-line inset
  const double lw = DT_PIXEL_APPLY_DPI(1.0);
  const double side = MIN(w, h) - lw;
  const double fx = x + lw * 0.5;
  const double fy = y + (h - MIN(w, h)) / 2.0 + lw * 0.5;
  const double r = DT_PIXEL_APPLY_DPI(2.0);
  cairo_new_sub_path(cr);
  cairo_arc(cr, fx + side - r, fy + r, r, -M_PI_2, 0.0);
  cairo_arc(cr, fx + side - r, fy + side - r, r, 0.0, M_PI_2);
  cairo_arc(cr, fx + r, fy + side - r, r, M_PI_2, M_PI);
  cairo_arc(cr, fx + r, fy + r, r, M_PI, 1.5 * M_PI);
  cairo_close_path(cr);
  cairo_set_line_width(cr, lw);
  cairo_stroke(cr);

  // text scaled to fit the padded interior, centred
  PangoLayout *layout = pango_cairo_create_layout(cr);
  if(darktable.bauhaus->pango_font_desc)
    pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_layout_set_text(layout, txt, -1);
  int tw = 0, th = 0;
  pango_layout_get_pixel_size(layout, &tw, &th);

  const double pad = DT_PIXEL_APPLY_DPI(1.0);
  const double avail = side - 2.0 * pad;
  const double scale = (tw > 0 && th > 0) ? fmin(avail / tw, avail / th) : 1.0;
  cairo_translate(cr, fx + (side - tw * scale) / 2.0, fy + (side - th * scale) / 2.0);
  cairo_scale(cr, scale, scale);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
  cairo_restore(cr);
}

// The single path the shrink/grow slider acts on, or NULL. We require an
// unambiguous target: a standalone path, the path explicitly selected inside a
// group, or - when nothing is explicitly selected - the group's only path.
// *out_index receives the form's position in the group (0 for a standalone
// path) for dt_masks_gui_form_create.
static dt_masks_form_t *_selected_single_path(int *out_index)
{
  if(out_index)
    *out_index = 0;
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  if(!form)
    return NULL;

  if(!(form->type & DT_MASKS_GROUP))
    return (form->type & DT_MASKS_PATH) ? form : NULL;

  // walk the group's shapes, tracking the sole path as we go: mask_form_selected_id
  // is only set once a shape is clicked, so right after creating or selecting a
  // single path it is still unset - fall back to that path if it is the only one.
  const gboolean has_selection = dt_is_valid_maskid(dev->mask_form_selected_id);
  dt_masks_form_t *single = NULL;
  int single_pos = 0;
  int npaths = 0;
  int pos = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts), pos++)
  {
    dt_masks_point_group_t *fpt = fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(dev, fpt->formid);
    if(!sel || !(sel->type & DT_MASKS_PATH))
      continue;

    // an explicit selection wins immediately
    if(has_selection && fpt->formid == dev->mask_form_selected_id)
    {
      if(out_index)
        *out_index = pos;
      return sel;
    }

    npaths++;
    single = sel;
    single_pos = pos;
  }

  // a specific shape was selected but it is not a path in this group
  if(has_selection)
    return NULL;

  if(npaths == 1)
  {
    if(out_index)
      *out_index = single_pos;
    return single;
  }
  return NULL;
}

static void _resize_cancel_pending(dt_lib_masks_t *d)
{
  if(d->resize_timer)
  {
    g_source_remove(d->resize_timer);
    d->resize_timer = 0;
  }
}

// Set the shape to the slider's absolute offset and commit one history item. The
// path mask owns the baseline and a per-offset result cache, so the morph runs
// (at most) once per distinct value and 0 restores the baseline.
static void _resize_commit(dt_lib_masks_t *d)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_gui_t *gui = dev->form_gui;
  int idx = 0;
  dt_masks_form_t *form = _selected_single_path(&idx);
  if(!form || !gui || !form->functions || !form->functions->resize)
    return;

  const int amount = (int)roundf(dt_bauhaus_slider_get(d->resize_amount));
  const gboolean pct = dt_bauhaus_widget_get_quad_active(d->resize_amount);

  if(!form->functions->resize(form, amount, pct) && amount < 0)
    dt_control_log(_("shrink amount too large: the path would disappear"));

  dt_masks_gui_form_create(form, gui, idx, dev->gui_module);
  dt_dev_add_masks_history_item(dev, dev->gui_module, TRUE);
  dt_control_queue_redraw_center();
}

static gboolean _resize_timeout(gpointer data)
{
  dt_lib_masks_t *d = data;
  d->resize_timer = 0;
  _resize_commit(d);
  return G_SOURCE_REMOVE;
}

// Debounce: morphing is expensive, so commit ~180 ms after the last change
// rather than on every slider tick.
static void _resize_schedule_commit(dt_lib_masks_t *d)
{
  if(d->resize_updating)
    return;
  if(d->resize_timer)
    g_source_remove(d->resize_timer);
  d->resize_timer = g_timeout_add(180, _resize_timeout, d);
}

static void _resize_amount_changed(GtkWidget *w, dt_lib_masks_t *d)
{
  _resize_schedule_commit(d);
}

// Reflect the current unit in the slider's value suffix (e.g. "5 px" / "5 %").
// Changing a lib widget's *label* at runtime re-registers its action and
// crashes (the first set_label moves the widget's module to its leaf action),
// so we use the value format, which is safe to update repeatedly. hard_max is
// 1000 (> 10) so "%" is a plain suffix, not the percentage auto-scaling case.
static void _resize_sync_unit(dt_lib_masks_t *d)
{
  const gboolean pct = dt_bauhaus_widget_get_quad_active(d->resize_amount);
  dt_bauhaus_slider_set_format(d->resize_amount, pct ? " %" : " px");
}

// the unit toggle lives in the slider's quad; bauhaus flips the active flag
// before emitting "quad-pressed", so the new state is read directly
static void _resize_unit_quad(GtkWidget *w, dt_lib_masks_t *d)
{
  const gboolean pct = dt_bauhaus_widget_get_quad_active(w);
  // keep the shared resize unit (also used by the scroll gesture) in sync,
  // storing the untranslated enum strings the path resize code matches against
  dt_conf_set_string("masks/path_resize_unit", pct ? "% of path size" : "pixels");
  _resize_sync_unit(d);
  _resize_schedule_commit(d);
}

// Refresh the shrink/grow controls for the current selection: show them only for
// a single path, and mirror the offset the path mask currently has applied (0 for
// a fresh shape, or whatever a scroll-wheel resize left). Querying the path mask
// keeps the slider truthful across both gestures without a second baseline here.
static void _resize_update(dt_lib_masks_t *d)
{
  int idx = 0;
  dt_masks_form_t *path = _selected_single_path(&idx);

  if(path)
  {
    const gboolean pct = dt_bauhaus_widget_get_quad_active(d->resize_amount);
    float amount = 0.0f;
    if(path->functions && path->functions->resize_get)
      path->functions->resize_get(path, pct, &amount);

    // reflect the current offset without triggering a (re)commit
    _resize_cancel_pending(d);
    d->resize_updating = TRUE;
    dt_bauhaus_slider_set(d->resize_amount, roundf(amount));
    d->resize_updating = FALSE;
  }
  gtk_widget_set_visible(d->resize_box, path != NULL);
}

static dt_masks_type_t _brush_conf_type(void)
{
  const dt_masks_form_t *form = darktable.develop->form_visible;
  if(form && (form->type & DT_MASKS_BRUSH)) return form->type;
  return DT_MASKS_BRUSH;
}

static void _brush_refresh_localadjust(void)
{
  dt_develop_t *dev = darktable.develop;
  if(dev && dev->proxy.localadjust.module)
    dt_lib_gui_queue_update(dev->proxy.localadjust.module);
}

static void _brush_size_changed(GtkWidget *w, dt_lib_masks_t *d)
{
  if(d->brush_updating) return;
  const float border = CLAMP(dt_bauhaus_slider_get(w) * 0.5f, 0.00005f, 0.5f);
  dt_conf_set_float(DT_MASKS_CONF(_brush_conf_type(), brush, border), border);
  dt_control_queue_redraw_center();
  _brush_refresh_localadjust();
}

static void _brush_feather_changed(GtkWidget *w, dt_lib_masks_t *d)
{
  if(d->brush_updating) return;
  const float hardness = CLAMP(1.0f - dt_bauhaus_slider_get(w), 0.0005f, 1.0f);
  dt_conf_set_float(DT_MASKS_CONF(_brush_conf_type(), brush, hardness), hardness);
  dt_control_queue_redraw_center();
  _brush_refresh_localadjust();
}

static void _brush_density_changed(GtkWidget *w, dt_lib_masks_t *d)
{
  if(d->brush_updating) return;
  const float density = CLAMP(dt_bauhaus_slider_get(w), 0.05f, 1.0f);
  dt_conf_set_float(DT_MASKS_CONF(_brush_conf_type(), brush, density), density);
  _brush_refresh_localadjust();
}

static void _brush_flow_changed(GtkWidget *w, dt_lib_masks_t *d)
{
  if(d->brush_updating) return;
  const float flow = CLAMP(dt_bauhaus_slider_get(w), 0.01f, 1.0f);
  dt_conf_set_float("plugins/darkroom/masks/brush/flow", flow);
  _brush_refresh_localadjust();
}

static void _brush_erase_changed(GtkWidget *w, dt_lib_masks_t *d)
{
  if(d->brush_updating) return;
  const gboolean erase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  // Arming refreshes the selection and brush panel synchronously. Publish
  // the requested mode first so that refresh cannot restore the old toggle.
  dt_conf_set_bool("plugins/darkroom/masks/brush/erase", erase);
  dt_mask_id_t id = NO_MASKID;
  dt_iop_module_t *owner = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
  GtkTreeModel *model = NULL;
  GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
  if(g_list_length(rows) == 1)
  {
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, rows->data))
      gtk_tree_model_get(model, &iter, TREE_MODULE, &owner, TREE_FORMID, &id, -1);
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  if(erase && !dt_masks_brush_start_erase(id, owner))
  {
    dt_conf_set_bool("plugins/darkroom/masks/brush/erase", FALSE);
    d->brush_updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), FALSE);
    d->brush_updating = FALSE;
    dt_control_log(_("select one brush component to erase"));
    return;
  }
}

static void _brush_panel_sync(dt_lib_masks_t *d, const gboolean show)
{
  if(!d->brush_box) return;
  /* no-show-all skips the box and every child. Clear it, show the
   * sliders, then put the flag back so a later show-all of the mask
   * manager cannot pop the panel open on its own. */
  if(show)
  {
    gtk_widget_set_no_show_all(d->brush_box, FALSE);
    gtk_widget_show_all(d->brush_box);
    gtk_widget_set_no_show_all(d->brush_box, TRUE);
  }
  else
    gtk_widget_hide(d->brush_box);
  if(!show) return;

  const dt_masks_type_t type = _brush_conf_type();
  const float border = dt_conf_get_float(DT_MASKS_CONF(type, brush, border));
  const float hardness = dt_conf_get_float(DT_MASKS_CONF(type, brush, hardness));
  const float density = dt_conf_get_float(DT_MASKS_CONF(type, brush, density));
  float flow = dt_conf_get_float("plugins/darkroom/masks/brush/flow");
  if(flow <= 0.0f) flow = 1.0f;

  d->brush_updating = TRUE;
  dt_bauhaus_slider_set(d->brush_size, CLAMP(border / 0.5f, 0.0002f, 1.0f));
  dt_bauhaus_slider_set(d->brush_feather, CLAMP(1.0f - hardness, 0.0f, 1.0f));
  dt_bauhaus_slider_set(d->brush_density, CLAMP(density <= 0.0f ? 1.0f : density, 0.05f, 1.0f));
  dt_bauhaus_slider_set(d->brush_flow, CLAMP(flow, 0.01f, 1.0f));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->brush_erase),
                               dt_conf_get_bool("plugins/darkroom/masks/brush/erase"));
  d->brush_updating = FALSE;
}

static void _update_all_properties(dt_lib_masks_t *self)
{
  gtk_widget_show(self->none_label);

  for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
    _property_changed(self->property[i], i);

  dt_masks_form_t *form = darktable.develop->form_visible;
  gboolean drawing_brush = form && form->type & DT_MASKS_BRUSH;
  if(form && (form->type & DT_MASKS_GROUP) && g_list_length(form->points) == 1)
  {
    const dt_mask_id_t id = ((dt_masks_point_group_t *)form->points->data)->formid;
    const dt_masks_form_t *selected = dt_masks_get_from_id(darktable.develop, id);
    drawing_brush = selected && (selected->type & DT_MASKS_BRUSH)
                    && !(selected->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE));
  }

  gtk_widget_set_visible(self->pressure, drawing_brush && darktable.gui->have_pen_pressure);
  gtk_widget_set_visible(self->smoothing, drawing_brush);
  _brush_panel_sync(self, drawing_brush);

  // shrink/grow applies only to a single path shape
  _resize_update(self);
}

static void _lib_masks_get_values(GtkTreeModel *model,
                                  GtkTreeIter *iter,
                                  dt_iop_module_t **module,
                                  dt_mask_id_t *groupid,
                                  dt_mask_id_t *formid)
{
  // returns module & groupid & formid if requested
  if(module ) gtk_tree_model_get(model, iter, TREE_MODULE, module, -1);
  if(groupid) gtk_tree_model_get(model, iter, TREE_GROUPID, groupid, -1);
  if(formid ) gtk_tree_model_get(model, iter, TREE_FORMID, formid, -1);
}

static gboolean _tree_row_matches_module(const dt_iop_module_t *row_module,
                                         const dt_iop_module_t *want,
                                         const int level)
{
  // top-level rows still match by formid only so group roots stay selectable.
  // children match the exact iop instance: two localmask groups can reuse one
  // leaf formid, and matching only the op name would pick the first of them.
  if(level == 1 || !want) return TRUE;
  return row_module == want;
}

static gboolean _form_is_group_member(const dt_mask_id_t formid)
{
  if(!dt_is_valid_maskid(formid)) return FALSE;
  for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    const dt_masks_form_t *grp = forms->data;
    if(!grp || !(grp->type & DT_MASKS_GROUP)) continue;
    for(const GList *points = grp->points; points; points = g_list_next(points))
    {
      const dt_masks_point_group_t *pt = points->data;
      if(pt && pt->formid == formid) return TRUE;
    }
  }
  return FALSE;
}

static gboolean _all_shapes_shown(const dt_lib_masks_t *lm)
{
  return lm && lm->all_shapes
         && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lm->all_shapes));
}

static void _lib_masks_inactivate_icons(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;

  // we set the add shape icons inactive
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_circle), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_ellipse), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_path), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_gradient), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_brush), FALSE);
#ifdef HAVE_AI
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_object), FALSE);
#endif
}

/* A click can run both the toggle signal and the press gesture. The
 * second call must not open the dialog again or create a second shape. */
static gboolean _shape_add_busy = FALSE;
static gboolean _shape_add_ok = FALSE;

static gboolean _shape_add_clear_busy(gpointer unused)
{
  _shape_add_busy = FALSE;
  _shape_add_ok = FALSE;
  return G_SOURCE_REMOVE;
}

static gboolean _tree_add_shape(GtkButton *button, gpointer shape)
{
  if(_shape_add_busy) return _shape_add_ok;
  _shape_add_busy = TRUE;

  dt_develop_t *dev = darktable.develop;
  dt_iop_module_t *module = NULL;

  /* Workspace shapes belong to a local mask. Another module's own
   * blend-toolbar buttons do not come through here. */
  if(dev->proxy.localadjust.module && dev->proxy.localadjust.begin_shape)
    module = dev->proxy.localadjust.begin_shape(dev->proxy.localadjust.module);

  // begin_shape may run a modal dialog's recursive main loop. Keep the guard
  // set throughout that loop, then clear it after this activation completes.
  g_idle_add(_shape_add_clear_busy, NULL);

  if(!module)
  {
    _shape_add_ok = FALSE;
    _lib_masks_inactivate_icons(dev->proxy.masks.module);
    return FALSE;
  }

  if(GPOINTER_TO_INT(shape) == DT_MASKS_BRUSH)
    dt_masks_brush_arm_new_component();

  dt_masks_form_t *spot = dt_masks_create(GPOINTER_TO_INT(shape));
  dt_masks_change_form_gui(spot);
  dev->form_gui->creation_module = module;
  dev->form_gui->group_selected = 0;
  dev->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  dt_control_queue_redraw_center();
  _brush_refresh_localadjust();
  _shape_add_ok = TRUE;
  return TRUE;
}

static gboolean _can_add_to_standard_group(const dt_iop_module_t *owner)
{
  return owner && !dt_iop_module_is(owner, "localmask")
         && !dt_iop_module_is(owner, "magiceraser")
         && !(owner->flags() & IOP_FLAGS_NO_MASKS)
         && owner->blend_data && owner->blend_params
         && (owner->blend_params->mask_mode & DEVELOP_MASK_MASK);
}

static gboolean _standard_group_is_exclusive(dt_iop_module_t *owner,
                                              const dt_mask_id_t groupid)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !owner || !g_list_find(dev->iop, owner)
     || !_can_add_to_standard_group(owner)
     || !(owner->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
     || !owner->enabled
     || (owner->blend_params->mask_mode & (DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER
                                          | DEVELOP_MASK_CONDITIONAL))
          != DEVELOP_MASK_ENABLED
     || owner->blend_params->mask_id != groupid)
    return FALSE;

  const dt_masks_form_t *group = dt_masks_get_from_id(dev, groupid);
  if(!group || group->type != DT_MASKS_GROUP || !group->points) return FALSE;

  // Keep this operation on a flat root that no other group or module uses.
  for(const GList *it = group->points; it; it = g_list_next(it))
  {
    const dt_masks_point_group_t *member = it->data;
    const dt_masks_form_t *child = member ? dt_masks_get_from_id(dev, member->formid) : NULL;
    if(!child || (child->type & DT_MASKS_GROUP)) return FALSE;
  }
  for(const GList *it = dev->forms; it; it = g_list_next(it))
  {
    const dt_masks_form_t *parent = it->data;
    if(parent == group || !(parent->type & DT_MASKS_GROUP)) continue;
    for(const GList *points = parent->points; points; points = g_list_next(points))
    {
      const dt_masks_point_group_t *member = points->data;
      if(member && member->formid == groupid) return FALSE;
    }
  }
  for(const GList *it = dev->iop; it; it = g_list_next(it))
  {
    const dt_iop_module_t *candidate = it->data;
    if(candidate != owner && candidate->blend_params
       && dt_masks_is_in_module(groupid, candidate))
      return FALSE;
  }
  return TRUE;
}

static gboolean _tree_add_shape_to_standard_group(dt_iop_module_t *owner,
                                                   gpointer shape)
{
  dt_develop_t *dev = darktable.develop;
  if(_shape_add_busy || !_can_add_to_standard_group(owner)
     || !g_list_find(dev->iop, owner)
     || (dev->form_gui && dev->form_gui->creation))
    return FALSE;

  _shape_add_busy = TRUE;
  g_idle_add(_shape_add_clear_busy, NULL);
  dt_iop_request_focus(owner);
  dt_iop_color_picker_reset(owner, FALSE);
  dt_masks_set_edit_mode(owner, DT_MASKS_EDIT_FULL);
  if(GPOINTER_TO_INT(shape) == DT_MASKS_BRUSH)
    dt_masks_brush_arm_new_component();

  dt_masks_form_t *spot = dt_masks_create(GPOINTER_TO_INT(shape));
  dt_masks_change_form_gui(spot);
  dev->form_gui->creation_module = owner;
  dev->form_gui->group_selected = 0;
  dev->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  dt_control_queue_redraw_center();
  _shape_add_ok = TRUE;
  return TRUE;
}

static gboolean _tree_add_shape_to_target(const dt_mask_id_t target,
                                          const gboolean group_context,
                                          const gboolean subtract,
                                          dt_iop_module_t *owner,
                                          const dt_masks_type_t shape)
{
  if(_shape_add_busy || (group_context && !dt_is_valid_maskid(target))) return FALSE;
  if(dt_is_valid_maskid(target))
  {
    dt_develop_t *dev = darktable.develop;
    const dt_masks_form_t *group = dt_masks_get_from_id(dev, target);
    if(!owner || !g_list_find(dev->iop, owner) || !owner->blend_params
       || owner->blend_params->mask_id != target
       || !group || group->type != DT_MASKS_GROUP
       || (subtract && (!group->points
                       || (owner->blend_params->mask_combine & DEVELOP_COMBINE_MASKS_POS)
                       || (!dt_iop_module_is(owner, "localmask")
                           && !_standard_group_is_exclusive(owner, target)))))
      return FALSE;

    if(!dt_iop_module_is(owner, "localmask"))
    {
      if(!_tree_add_shape_to_standard_group(owner, GINT_TO_POINTER(shape))) return FALSE;
      if(!subtract) return TRUE;
    }
    else
    {
      if(!dev->proxy.localadjust.module || !dev->proxy.localadjust.select_mask
         || !dev->proxy.localadjust.selected_mask)
        return FALSE;
      dev->proxy.localadjust.select_mask(dev->proxy.localadjust.module, owner);
      if(dev->proxy.localadjust.selected_mask(dev->proxy.localadjust.module) != owner)
        return FALSE;
    }
  }
  else if(subtract)
    return FALSE;

  if((!dt_is_valid_maskid(target) || dt_iop_module_is(owner, "localmask"))
     && !_tree_add_shape(NULL, GINT_TO_POINTER(shape)))
    return FALSE;
  if(!subtract) return TRUE;

  dt_develop_t *dev = darktable.develop;
  const dt_masks_form_t *group = dt_masks_get_from_id(dev, target);
  if(!dev->form_gui || !dev->form_gui->creation
     || !g_list_find(dev->iop, owner)
     || dev->form_gui->creation_module != owner
     || !group || !(group->type & DT_MASKS_GROUP) || !group->points
     || owner->blend_params->mask_id != target
     || (owner->blend_params->mask_combine & DEVELOP_COMBINE_MASKS_POS)
     || (!dt_iop_module_is(owner, "localmask")
         && !_standard_group_is_exclusive(owner, target)))
  {
    dt_masks_cancel_creation();
    return FALSE;
  }
  dev->form_gui->subtract_parentid = target;
  if(dt_iop_module_is(owner, "localmask")
     && (!dev->proxy.localadjust.hold_shape_undo
         || !dev->proxy.localadjust.hold_shape_undo(dev->proxy.localadjust.module, owner, target)))
  {
    dt_masks_cancel_creation();
    return FALSE;
  }
  return TRUE;
}

static void _tree_add_shape_menu(GtkButton *button, gpointer shape)
{
  const dt_mask_id_t target = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "target-group-id"));
  const gboolean group_context = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "group-context"));
  const gboolean subtract = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "subtract-from-mask"));
  dt_iop_module_t *owner = g_object_get_data(G_OBJECT(button), "target-owner");
  _tree_add_shape_to_target(target, group_context, subtract, owner,
                            (dt_masks_type_t)GPOINTER_TO_INT(shape));
}

static void _tree_append_subtract_tool(GtkWidget *menu, const gchar *label,
                                       const dt_masks_type_t type, const dt_mask_id_t groupid,
                                       dt_iop_module_t *owner)
{
  GtkWidget *item = gtk_menu_item_new_with_label(label);
  g_object_set_data(G_OBJECT(item), "target-group-id", GINT_TO_POINTER(groupid));
  g_object_set_data(G_OBJECT(item), "target-owner", owner);
  g_object_set_data(G_OBJECT(item), "group-context", GINT_TO_POINTER(TRUE));
  g_object_set_data(G_OBJECT(item), "subtract-from-mask", GINT_TO_POINTER(TRUE));
  g_signal_connect(item, "activate", G_CALLBACK(_tree_add_shape_menu), GINT_TO_POINTER(type));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void _bt_add_shape_cb(GtkGestureSingle *gesture, int n_press, double x, double y, gpointer shape)
{

  if(dt_gui_current_button(gesture) == GDK_BUTTON_PRIMARY)
  {
#ifdef HAVE_AI
    if(GPOINTER_TO_INT(shape) == DT_MASKS_OBJECT && !dt_masks_object_available())
    {
      dt_control_log(_("AI model is not available. Check preferences > AI"));
      return;
    }
#endif
    if(!_tree_add_shape(NULL, shape)) return;

    if(dt_modifier_is(dt_gui_current_state(gesture), GDK_CONTROL_MASK))
    {
      darktable.develop->form_gui->creation_continuous = TRUE;
      darktable.develop->form_gui->creation_continuous_module =
        darktable.develop->form_gui->creation_module;
    }

    _lib_masks_inactivate_icons(darktable.develop->proxy.masks.module);
  }
}

/* The shape button is a toggle. A click can light the button without the
 * press gesture running, which leaves no brush form and therefore no
 * panel. If the brush is not already armed, arm it, then show the panel. */
static void _brush_button_toggled(GtkToggleButton *button, dt_lib_masks_t *d)
{
  if(d->brush_updating) return;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button))) return;

  dt_develop_t *dev = darktable.develop;
  const dt_masks_form_t *form = dev ? dev->form_visible : NULL;
  const gboolean armed = form && (form->type & DT_MASKS_BRUSH)
                         && dev->form_gui && dev->form_gui->creation;
  if(!armed && !_tree_add_shape(NULL, GINT_TO_POINTER(DT_MASKS_BRUSH)))
    return;
  _brush_panel_sync(d, TRUE);
}

static void _tree_add_exist(GtkButton *button, dt_masks_form_t *grp)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;
  // we get the new formid
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(button), "module");

  // we add the form in this group
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(form && dt_masks_group_add_form(grp, form))
  {
    // A newly populated localmask needs its drawn mode in the same snapshot.
    const gboolean mode_changed = dt_masks_iop_sync_localmask_mode(module);
    dt_dev_add_masks_history_item(darktable.develop,
                                  mode_changed ? module : NULL, FALSE);

    // and we apply the change
    dt_masks_iop_update(module);
    dt_dev_masks_selection_change(darktable.develop, NULL, grp->formid);
  }
}

static void _tree_group(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;
  // we create the new group
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  snprintf(grp->name, sizeof(grp->name), _("group #%d"),
           g_list_length(darktable.develop->forms));

  // we add all selected forms to this group
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  int pos = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, NULL, NULL, &id);

      if(dt_is_valid_maskid(id))
      {
        dt_masks_point_group_t *fpt = malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = id;
        fpt->parentid = grp->formid;
        fpt->opacity = 1.0f;
        fpt->state = DT_MASKS_STATE_USE;
        if(pos > 0) fpt->state |= DT_MASKS_STATE_UNION;
        grp->points = g_list_append(grp->points, fpt);
        pos++;
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  // we add this group to the general list
  darktable.develop->forms = g_list_append(darktable.develop->forms, grp);

  // add we save
  dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
  _lib_masks_recreate_list(self);
  // dt_masks_change_form_gui(grp);
}

static void _set_iter_name(dt_lib_masks_t *lm,
                           dt_masks_form_t *form,
                           const int state,
                           const float opacity,
                           GtkTreeModel *model,
                           GtkTreeIter *iter)
{
  if(!form) return;

  dt_iop_module_t *module = NULL;
  dt_mask_id_t groupid = NO_MASKID;
  gtk_tree_model_get(model, iter, TREE_MODULE, &module, TREE_GROUPID, &groupid, -1);

  char str[256] = "";
  // other-module groups such as exposure stay in this tree; prefix the owner
  // so they are not mistaken for a localmask parent
  if((form->type & DT_MASKS_GROUP) && groupid == NO_MASKID && module
     && !dt_iop_module_is(module, "localmask"))
  {
    gchar *module_label = dt_history_item_get_name(module);
    snprintf(str, sizeof(str), "%s: %s", module_label, form->name);
    g_free(module_label);
  }
  else
    g_strlcat(str, form->name, sizeof(str));

  if(opacity != 1.0f)
  {
    char str2[256] = "";
    g_strlcpy(str2, str, sizeof(str2));
    snprintf(str, sizeof(str), "%s %d%%", str2, (int)(opacity * 100));
  }

  const gboolean show = state & DT_MASKS_STATE_SHOW;

  GdkPixbuf *icop = NULL;
  GdkPixbuf *icinv = NULL;

  if(state & DT_MASKS_STATE_UNION)
    icop = lm->ic_union;
  else if(state & DT_MASKS_STATE_INTERSECTION)
    icop = lm->ic_intersection;
  else if(state & DT_MASKS_STATE_DIFFERENCE)
    icop = lm->ic_difference;
  else if(state & DT_MASKS_STATE_SUM)
    icop = lm->ic_sum;
  else if(state & DT_MASKS_STATE_EXCLUSION)
    icop = lm->ic_exclusion;

  if(state & DT_MASKS_STATE_INVERSE)
    icinv = lm->ic_inverse;

  gtk_tree_store_set(GTK_TREE_STORE(model), iter,
                     TREE_TEXT, str,
                     TREE_IC_OP, icop,
                     TREE_IC_OP_VISIBLE, (icop != NULL) && show,
                     TREE_IC_INVERSE, icinv,
                     TREE_IC_INVERSE_VISIBLE, (icinv != NULL),
                     -1);
}

static void _tree_cleanup(GtkButton *button, dt_lib_module_t *self)
{
  dt_masks_cleanup_unused(darktable.develop);
  _lib_masks_recreate_list(self);
}

static void _add_masks_history_item(dt_lib_masks_t *lm)
{
  DT_ENTER_GUI_UPDATE();
  dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
  DT_LEAVE_GUI_UPDATE();
}


static void _tree_operation(GtkButton *button, gpointer user_data)
{
  dt_masks_state_t change_state = GPOINTER_TO_INT(user_data);
  dt_lib_module_t *self = darktable.develop->proxy.masks.module;
  dt_lib_masks_t *lm = self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  gboolean change = FALSE;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

  for(const GList *items_iter = items;
      items_iter;
      items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;

    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      dt_mask_id_t grid = INVALID_MASKID;
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_point_group_t *pt = pts->data;
          if(pt->formid == id)
          {
            if(change_state == DT_MASKS_STATE_INVERSE
               || (pt->state & DT_MASKS_STATE_OP
                   && !(pt->state & change_state)))
            {
              if(change_state != DT_MASKS_STATE_INVERSE)
                pt->state &= ~DT_MASKS_STATE_OP;
              pt->state ^= change_state;
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id),
                             pt->state, pt->opacity, model,
                             &iter);
              change = TRUE;
            }
            break;
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  if(change)
  {
    _add_masks_history_item(lm);
    _overlay_set_selected(selection);
  }
}

static void _add_tree_operation(GtkMenuShell *menu,
                                gchar *label,
                                dt_masks_state_t state,
                                dt_masks_state_t selected_states,
                                gboolean sensitive)
{
  GtkWidget *item = gtk_check_menu_item_new_with_label(label);
  gtk_widget_set_sensitive(item, sensitive);
  if(selected_states & state)
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
  g_signal_connect(item, "activate", G_CALLBACK(_tree_operation),
                    GINT_TO_POINTER(state));
  gtk_menu_shell_append(menu, item);
}

static void _swap_last_secondlast_item_visibility(dt_lib_masks_t *lm,
                                                   GtkTreeIter *iter,
                                                   const dt_mask_id_t secondlast_id,
                                                   const dt_mask_id_t last_id)
{
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));

  dt_mask_id_t grid = INVALID_MASKID;
  dt_mask_id_t id = INVALID_MASKID;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);

  if(grp)
  {
    // we search the entries and change the state
    // the new last entry is removed the SHOW state and
    // the new second last node is set SHOW state + UNION if no operator defined yet.
    for(const GList *pts = g_list_last(grp->points); pts; pts = g_list_previous(pts))
    {
      dt_masks_point_group_t *pt = pts->data;
      gboolean changed = FALSE;
      if(pt->formid == last_id)
      {
        pt->state &= ~DT_MASKS_STATE_SHOW;
        changed = TRUE;
      }
      else if(pt->formid == secondlast_id)
      {
        // ensure that at least an operator is defined as we are
        // going to show this mask operator.
        if((pt->state & DT_MASKS_STATE_OP) == DT_MASKS_STATE_NONE)
          pt->state |= DT_MASKS_STATE_UNION;
        pt->state |= DT_MASKS_STATE_SHOW;
        changed = TRUE;
      }
      if(changed)
        _set_iter_name(lm,
                       dt_masks_get_from_id(darktable.develop, id),
                       pt->state, pt->opacity, model,
                       iter);
    }
  }
}

static gboolean _is_last_tree_item(GtkTreeModel *model, GtkTreeIter *iter)
{
  GtkTreeIter *tmp = gtk_tree_iter_copy(iter);
  const gboolean is_last_item = !gtk_tree_model_iter_next(model, tmp);
  gtk_tree_iter_free(tmp);
  return is_last_item;
}

static void _tree_moveup(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;

  dt_masks_clear_form_gui(darktable.develop);

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

  for(const GList *items_iter = items;
      items_iter;
      items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;

    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      dt_mask_id_t grid = INVALID_MASKID;
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      GtkTreeIter *prev_iter = gtk_tree_iter_copy(&iter);
      if(gtk_tree_model_iter_previous(model, prev_iter))
      {
        dt_mask_id_t prev_grid = INVALID_MASKID;
        dt_mask_id_t prev_id = INVALID_MASKID;
        _lib_masks_get_values(model, prev_iter, NULL, &prev_grid, &prev_id);

        if(_is_last_tree_item(model, &iter))
        {
          _swap_last_secondlast_item_visibility(lm, &iter, id, prev_id);
        }
      }

      gtk_tree_iter_free(prev_iter);

      dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 1);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
  _lib_masks_recreate_list(self);
}

static void _tree_movedown(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;

  dt_masks_clear_form_gui(darktable.develop);

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

  for(const GList *items_iter = items;
      items_iter;
      items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;

    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      dt_mask_id_t grid = INVALID_MASKID;
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      GtkTreeIter *next_iter = gtk_tree_iter_copy(&iter);
      gtk_tree_model_iter_next(model, next_iter);
      dt_mask_id_t next_grid = INVALID_MASKID;
      dt_mask_id_t next_id = INVALID_MASKID;
      _lib_masks_get_values(model, next_iter, NULL, &next_grid, &next_id);

      if(_is_last_tree_item(model, next_iter))
      {
        _swap_last_secondlast_item_visibility(lm, &iter, next_id, id);
      }

      gtk_tree_iter_free(next_iter);

      dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 0);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
  _lib_masks_recreate_list(self);
}

static void _tree_delete_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;

  dt_masks_clear_form_gui(darktable.develop);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  dt_iop_module_t *module = NULL;

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);

  for(const GList *items_iter = items;
      items_iter;
      items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      GtkTreeIter *prev_iter = gtk_tree_iter_copy(&iter);
      GtkTreeIter *next_iter = gtk_tree_iter_copy(&iter);
      const gboolean has_previous = gtk_tree_model_iter_previous(model, prev_iter);
      const gboolean has_next = gtk_tree_model_iter_next(model, next_iter);
      dt_mask_id_t prev_grid = INVALID_MASKID;
      dt_mask_id_t prev_id = INVALID_MASKID;

      dt_mask_id_t grid = INVALID_MASKID;
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, &module, &grid, &id);

      if(has_previous)
        gtk_tree_selection_select_iter(selection, prev_iter);
      else if(has_next)
        gtk_tree_selection_select_iter(selection, next_iter);

      if(has_previous)
      {
        _lib_masks_get_values(model, prev_iter, NULL, &prev_grid, &prev_id);
        if(_is_last_tree_item(model, &iter))
        {
          _swap_last_secondlast_item_visibility(lm, &iter, id, prev_id);
        }
      }
      gtk_tree_iter_free(prev_iter);
      gtk_tree_iter_free(next_iter);
      dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, grid),
                           dt_masks_get_from_id(darktable.develop, id));
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
  dt_undo_end_group(darktable.undo);
  _lib_masks_recreate_list(self);
}

typedef struct dt_lib_masks_shape_context_t
{
  dt_mask_id_t formid;
  dt_mask_id_t groupid;
  dt_iop_module_t *owner;
  dt_masks_state_t state;
  float opacity;
} dt_lib_masks_shape_context_t;

static gboolean _shape_has_other_use(const dt_mask_id_t formid,
                                     const dt_mask_id_t source_groupid)
{
  dt_develop_t *dev = darktable.develop;
  for(const GList *it = dev->forms; it; it = g_list_next(it))
  {
    const dt_masks_form_t *group = it->data;
    if(!(group->type & DT_MASKS_GROUP) || group->formid == source_groupid) continue;
    for(const GList *points = group->points; points; points = g_list_next(points))
    {
      const dt_masks_point_group_t *point = points->data;
      if(point->formid == formid) return TRUE;
    }
  }
  for(const GList *it = dev->iop; it; it = g_list_next(it))
  {
    const dt_iop_module_t *module = it->data;
    if(module->blend_params && module->blend_params->mask_id == formid) return TRUE;
  }
  return FALSE;
}

static gboolean _shape_context(GtkTreeModel *model, GtkTreePath *path,
                               dt_lib_masks_shape_context_t *context)
{
  dt_develop_t *dev = darktable.develop;
  const int depth = gtk_tree_path_get_depth(path);
  if(!dev || !dev->gui_attached || (depth != 1 && depth != 2)) return FALSE;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, path)) return FALSE;
  dt_mask_id_t groupid = NO_MASKID;
  dt_mask_id_t formid = NO_MASKID;
  _lib_masks_get_values(model, &iter, NULL, &groupid, &formid);
  const dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(!form || !(form->type == DT_MASKS_BRUSH || form->type == DT_MASKS_ELLIPSE
                || form->type == DT_MASKS_PATH || form->type == DT_MASKS_GRADIENT
                || form->type == DT_MASKS_CIRCLE))
    return FALSE;

  *context = (dt_lib_masks_shape_context_t){
    .formid = formid,
    .groupid = NO_MASKID,
    .owner = NULL,
    .state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE,
    .opacity = dt_conf_get_float("plugins/darkroom/masks/opacity")
  };
  if(depth == 1)
    return !_form_is_group_member(formid)
           && !_shape_has_other_use(formid, NO_MASKID);

  const dt_masks_form_t *group = dt_masks_get_from_id(dev, groupid);
  if(!group || group->type != DT_MASKS_GROUP) return FALSE;
  // A shared root would change every owner when one child is removed.
  for(const GList *it = dev->forms; it; it = g_list_next(it))
  {
    const dt_masks_form_t *parent = it->data;
    if(parent == group || !(parent->type & DT_MASKS_GROUP)) continue;
    for(const GList *points = parent->points; points; points = g_list_next(points))
    {
      const dt_masks_point_group_t *point = points->data;
      if(point->formid == groupid) return FALSE;
    }
  }
  for(const GList *it = dev->iop; it; it = g_list_next(it))
  {
    dt_iop_module_t *module = it->data;
    if(!module->blend_params || module->blend_params->mask_id != groupid) continue;
    if(context->owner) return FALSE;
    context->owner = module;
  }
  // only local adjustments own the remove/delete semantics used below
  if(!context->owner || !dt_iop_module_is(context->owner, "localmask")
     || context->owner->multi_priority <= 0) return FALSE;
  guint occurrences = 0;
  for(const GList *points = group->points; points; points = g_list_next(points))
  {
    const dt_masks_point_group_t *point = points->data;
    if(point->formid != formid) continue;
    if(++occurrences > 1) return FALSE;
    context->groupid = groupid;
    context->state = point->state;
    context->opacity = point->opacity;
  }
  return occurrences == 1;
}

static void _tree_create_mask_from_shape(GtkMenuItem *item, dt_lib_module_t *self)
{
  (void)item;
  dt_develop_t *dev = darktable.develop;
  dt_lib_masks_t *lm = self->data;
  if(!dev || !dev->gui_attached || !darktable.undo
     || darktable.undo->group != DT_UNDO_NONE
     || (dev->form_gui && dev->form_gui->creation)) return;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  dt_lib_masks_shape_context_t source;
  const gboolean valid = items && !items->next
                         && _shape_context(model, items->data, &source);
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  if(!valid) return;

  dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop, "localmask", 0);
  const dt_masks_form_t *form = dt_masks_get_from_id(dev, source.formid);
  if(!base || !form) return;

  dt_masks_form_t *group = dt_masks_create(DT_MASKS_GROUP);
  if(!group) return;
  while(dt_masks_get_from_id(dev, group->formid)) group->formid++;
  dt_masks_point_group_t *point = dt_masks_group_add_form(group, form);
  if(!point)
  {
    dt_masks_free_form(group);
    return;
  }
  point->state = source.state & ~DT_MASKS_STATE_OP;
  point->opacity = source.opacity;

  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  dt_iop_module_t *mask = dt_iop_gui_duplicate(base, FALSE);
  if(!mask || !mask->blend_params)
  {
    if(mask) dt_iop_gui_delete(mask);
    dt_masks_free_form(group);
    dt_undo_end_group(darktable.undo);
    return;
  }

  gchar *label = dt_history_item_get_name(mask);
  snprintf(group->name, sizeof(group->name), _("group `%s'"), label);
  g_free(label);
  dt_pthread_mutex_lock(&dev->history_mutex);
  dev->forms = g_list_append(dev->forms, group);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_develop_blend_params_t bp = *mask->blend_params;
  bp.mask_id = group->formid;
  bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  bp.mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;
  dt_iop_commit_blend_params(mask, &bp, NULL);
  dt_dev_add_masks_history_item(dev, mask, TRUE);

  if(dt_is_valid_maskid(source.groupid))
  {
    dt_masks_form_t *old_group = dt_masks_get_from_id(dev, source.groupid);
    dt_masks_form_t *old_form = dt_masks_get_from_id(dev, source.formid);
    if(old_group && old_form)
      dt_masks_form_remove(source.owner, old_group, old_form);
  }
  dt_undo_end_group(darktable.undo);

  dt_iop_gui_update_blending(mask);
  if(dev->proxy.localadjust.module && dev->proxy.localadjust.select_mask)
    dev->proxy.localadjust.select_mask(dev->proxy.localadjust.module, mask);
  dt_masks_set_edit_mode_single_form(mask, source.formid, DT_MASKS_EDIT_FULL);
  dt_dev_masks_selection_change(dev, mask, source.formid);
  _lib_masks_recreate_list(self);
}

static void _tree_delete_component(GtkMenuItem *item, dt_lib_module_t *self)
{
  (void)item;
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !darktable.undo
     || darktable.undo->group != DT_UNDO_NONE
     || (dev->form_gui && dev->form_gui->creation)) return;
  dt_lib_masks_t *lm = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  GArray *targets = g_array_new(FALSE, FALSE, sizeof(dt_lib_masks_shape_context_t));
  gboolean valid = items != NULL;
  for(const GList *it = items; it; it = g_list_next(it))
  {
    dt_lib_masks_shape_context_t context;
    if(!_shape_context(model, it->data, &context)
       || !dt_is_valid_maskid(context.groupid))
    {
      valid = FALSE;
      break;
    }
    g_array_append_val(targets, context);
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  if(!valid)
  {
    g_array_free(targets, TRUE);
    return;
  }

  dt_masks_clear_form_gui(darktable.develop);
  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  for(guint i = 0; i < targets->len; i++)
  {
    const dt_lib_masks_shape_context_t target =
      g_array_index(targets, dt_lib_masks_shape_context_t, i);
    dt_masks_form_t *group = dt_masks_get_from_id(darktable.develop, target.groupid);
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, target.formid);
    if(!group || !form) continue;
    if(_shape_has_other_use(target.formid, target.groupid))
      dt_masks_form_remove(target.owner, group, form);
    else
      dt_masks_form_remove(target.owner, NULL, form);
  }
  dt_undo_end_group(darktable.undo);
  g_array_free(targets, TRUE);
  _lib_masks_recreate_list(self);
}

static void _tree_duplicate_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;

  // we get the selected node
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  if(!items) return;
  GtkTreePath *item = (GtkTreePath *)items->data;
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(model, &iter, item))
  {
    dt_mask_id_t id = INVALID_MASKID;
    _lib_masks_get_values(model, &iter, NULL, NULL, &id);

    const dt_mask_id_t nid = dt_masks_form_duplicate(darktable.develop, id);
    if(dt_is_valid_maskid(nid))
    {
      dt_dev_masks_selection_change(darktable.develop, NULL, nid);
      //_lib_masks_recreate_list(self);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
}

static void _tree_cell_edited(GtkCellRendererText *cell,
                              gchar *path_string,
                              gchar *new_text,
                              dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(model, &iter, path_string)) return;

  dt_mask_id_t id = INVALID_MASKID;
  _lib_masks_get_values(model, &iter, NULL, NULL, &id);
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form) return;

  // we want to make sure that the new name is not an empty
  // string. else this would convert in the xmp file into "<rdf:li/>"
  // which produces problems. we use a single whitespace as the pure
  // minimum text.
  gchar *text = strlen(new_text) == 0 ? " " : new_text;

  // first, we need to update the mask name

  g_strlcpy(form->name, text, sizeof(form->name));
  dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
}

static dt_iop_module_t *_selection_owner(GtkTreeModel *model, const GList *items)
{
  if(!items || items->next) return NULL;

  // a reused leaf is disambiguated by the top-level group the user selected it under
  GtkTreePath *root_path = gtk_tree_path_copy(items->data);
  const gboolean in_group = gtk_tree_path_get_depth(root_path) > 1;
  while(gtk_tree_path_get_depth(root_path) > 1) gtk_tree_path_up(root_path);
  GtkTreeIter root_iter;
  dt_mask_id_t root_id = NO_MASKID;
  if(gtk_tree_model_get_iter(model, &root_iter, root_path))
    _lib_masks_get_values(model, &root_iter, NULL, NULL, &root_id);
  gtk_tree_path_free(root_path);
  if(!dt_is_valid_maskid(root_id)) return NULL;

  const dt_masks_form_t *root = dt_masks_get_from_id(darktable.develop, root_id);
  const gboolean exact_root = in_group || (root && (root->type & DT_MASKS_GROUP));
  dt_iop_module_t *owner = NULL;
  for(const GList *it = darktable.develop->iop; it; it = g_list_next(it))
  {
    dt_iop_module_t *module = it->data;
    if(!module->blend_params) continue;
    const gboolean uses = exact_root ? module->blend_params->mask_id == root_id
                                    : dt_masks_is_in_module(root_id, module);
    if(!uses) continue;
    // disabled and legacy users count too: a shared root has no unique owner
    if(owner) return NULL;
    owner = module;
  }
  /* Magic Eraser stores a brush selection in blend_params, but its accepted
     patch already carries coverage and must not be output-blended again. */
  return owner && (owner->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    && (!(owner->flags() & IOP_FLAGS_NO_MASKS)
        || dt_iop_module_is(owner, "magiceraser")) ? owner : NULL;
}

static dt_iop_module_t *_selected_group_owner(GtkTreeModel *model,
                                               const GList *items,
                                               dt_mask_id_t *groupid)
{
  if(!items || items->next || gtk_tree_path_get_depth(items->data) != 1) return NULL;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, items->data)) return NULL;
  dt_mask_id_t id = NO_MASKID;
  _lib_masks_get_values(model, &iter, NULL, NULL, &id);
  const dt_masks_form_t *group = dt_masks_get_from_id(darktable.develop, id);
  if(!group || group->type != DT_MASKS_GROUP) return NULL;

  dt_iop_module_t *owner = _selection_owner(model, items);
  if(!owner || !owner->blend_params || owner->blend_params->mask_id != id)
    return NULL;
  if(groupid) *groupid = id;
  return owner;
}

static dt_iop_module_t *_selected_localmask_group_owner(GtkTreeModel *model,
                                                         const GList *items,
                                                         dt_mask_id_t *groupid)
{
  dt_iop_module_t *owner = _selected_group_owner(model, items, groupid);
  return owner && dt_iop_module_is(owner, "localmask") ? owner : NULL;
}

static dt_iop_module_t *_selected_localmask_component(GtkTreeModel *model,
                                                       const GList *items,
                                                       dt_masks_form_t **group,
                                                       dt_masks_point_group_t **point,
                                                       dt_mask_id_t *formid)
{
  if(!items || items->next || gtk_tree_path_get_depth(items->data) != 2) return NULL;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, items->data)) return NULL;
  dt_mask_id_t groupid = NO_MASKID;
  dt_mask_id_t id = NO_MASKID;
  _lib_masks_get_values(model, &iter, NULL, &groupid, &id);
  dt_iop_module_t *owner = _selection_owner(model, items);
  dt_masks_form_t *parent = dt_masks_get_from_id(darktable.develop, groupid);
  const dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, id);
  if(!owner || !dt_iop_module_is(owner, "localmask")
     || owner->blend_params->mask_id != groupid
     || !parent || !(parent->type & DT_MASKS_GROUP)
     || !child || !child->functions || !child->functions->duplicate_points
     || !(child->type == DT_MASKS_BRUSH || child->type == DT_MASKS_ELLIPSE
          || child->type == DT_MASKS_PATH || child->type == DT_MASKS_GRADIENT
          || child->type == DT_MASKS_CIRCLE))
    return NULL;

  for(const GList *it = parent->points; it; it = g_list_next(it))
  {
    dt_masks_point_group_t *member = it->data;
    if(member->formid == id)
    {
      if(group) *group = parent;
      if(point) *point = member;
      if(formid) *formid = id;
      return owner;
    }
  }
  return NULL;
}

static gboolean _copy_drag_group_is_safe(dt_iop_module_t *owner,
                                         const dt_mask_id_t groupid)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !owner || !g_list_find(dev->iop, owner)
     || !owner->enabled || !owner->blend_params
     || !dt_iop_module_is(owner, "localmask")
     || owner->blend_params->mask_id != groupid
     || (owner->blend_params->mask_mode & (DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK
                                          | DEVELOP_MASK_RASTER | DEVELOP_MASK_CONDITIONAL))
          != (DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK)
     || (dev->form_gui && dev->form_gui->creation))
    return FALSE;

  const dt_masks_form_t *group = dt_masks_get_from_id(dev, groupid);
  if(!group || group->type != DT_MASKS_GROUP || !group->points) return FALSE;
  for(const GList *it = group->points; it; it = g_list_next(it))
  {
    const dt_masks_point_group_t *member = it->data;
    const dt_masks_form_t *child = member ? dt_masks_get_from_id(dev, member->formid) : NULL;
    if(!child || !(child->type == DT_MASKS_BRUSH || child->type == DT_MASKS_ELLIPSE
                   || child->type == DT_MASKS_PATH || child->type == DT_MASKS_GRADIENT
                   || child->type == DT_MASKS_CIRCLE))
      return FALSE;
  }
  for(const GList *it = dev->forms; it; it = g_list_next(it))
  {
    const dt_masks_form_t *parent = it->data;
    if(parent == group || !(parent->type & DT_MASKS_GROUP)) continue;
    for(const GList *points = parent->points; points; points = g_list_next(points))
    {
      const dt_masks_point_group_t *member = points->data;
      if(member && member->formid == groupid) return FALSE;
    }
  }
  return TRUE;
}

static dt_iop_module_t *_copy_drag_group_owner(const dt_mask_id_t groupid)
{
  dt_iop_module_t *owner = NULL;
  for(const GList *it = darktable.develop->iop; it; it = g_list_next(it))
  {
    dt_iop_module_t *candidate = it->data;
    if(candidate->blend_params && candidate->blend_params->mask_id == groupid)
    {
      if(owner) return NULL;
      owner = candidate;
    }
  }
  return _copy_drag_group_is_safe(owner, groupid) ? owner : NULL;
}

static dt_masks_point_group_t *_copy_drag_source_point(const dt_mask_id_t groupid,
                                                       const dt_mask_id_t formid)
{
  dt_develop_t *dev = darktable.develop;
  const dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(!form || !form->functions || !form->functions->duplicate_points
     || !(form->type == DT_MASKS_BRUSH || form->type == DT_MASKS_ELLIPSE
          || form->type == DT_MASKS_PATH || form->type == DT_MASKS_GRADIENT
          || form->type == DT_MASKS_CIRCLE))
    return NULL;

  dt_masks_point_group_t *source = NULL;
  for(const GList *it = dev->forms; it; it = g_list_next(it))
  {
    const dt_masks_form_t *group = it->data;
    if(!(group->type & DT_MASKS_GROUP)) continue;
    for(const GList *points = group->points; points; points = g_list_next(points))
    {
      dt_masks_point_group_t *member = points->data;
      if(!member) return NULL;
      if(member->formid != formid) continue;
      if(group->formid != groupid || source) return NULL;
      source = member;
    }
  }
  for(const GList *it = dev->iop; it; it = g_list_next(it))
  {
    const dt_iop_module_t *module = it->data;
    if(module->blend_params && module->blend_params->mask_id == formid)
      return NULL;
  }
  return source;
}

static dt_iop_module_t *_copy_drag_target(GtkTreeView *treeview,
                                           const gint x, const gint y,
                                           dt_mask_id_t *groupid,
                                           GtkTreePath **root_path)
{
  GtkTreePath *path = NULL;
  if(!gtk_tree_view_get_path_at_pos(treeview, x, y, &path, NULL, NULL, NULL))
    return NULL;
  while(gtk_tree_path_get_depth(path) > 1) gtk_tree_path_up(path);
  GtkTreeModel *model = gtk_tree_view_get_model(treeview);
  GList row = { .data = path };
  dt_iop_module_t *owner = _selected_localmask_group_owner(model, &row, groupid);
  if(owner && !_copy_drag_group_is_safe(owner, *groupid)) owner = NULL;
  if(owner && root_path) *root_path = path;
  else gtk_tree_path_free(path);
  return owner;
}

static void _tree_copy_drag_begin(GtkGestureDrag *gesture, double x, double y,
                                  dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;
  lm->copy_drag_groupid = lm->copy_drag_formid = NO_MASKID;
  lm->copy_drag_start_x = x;
  lm->copy_drag_start_y = y;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_PRIMARY
     || !dt_modifier_is(dt_gui_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)),
                        GDK_MOD1_MASK))
  {
    dt_gui_deny(gesture);
    return;
  }

  GtkTreeView *treeview = GTK_TREE_VIEW(lm->treeview);
  GtkTreePath *path = NULL;
  if(gtk_tree_view_get_path_at_pos(treeview, (gint)x, (gint)y,
                                   &path, NULL, NULL, NULL))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(treeview);
    GList row = { .data = path };
    dt_masks_form_t *group = NULL;
    dt_mask_id_t formid = NO_MASKID;
    dt_iop_module_t *owner = _selected_localmask_component(model, &row,
                                                             &group, NULL, &formid);
    if(owner && _copy_drag_group_is_safe(owner, group->formid)
       && _copy_drag_source_point(group->formid, formid))
    {
      lm->copy_drag_groupid = group->formid;
      lm->copy_drag_formid = formid;
    }
    gtk_tree_path_free(path);
  }
  if(dt_is_valid_maskid(lm->copy_drag_formid)) dt_gui_claim(gesture);
  else dt_gui_deny(gesture);
}

static void _tree_copy_drag_update(GtkGestureDrag *gesture, double offset_x,
                                   double offset_y, dt_lib_module_t *self)
{
  (void)gesture;
  dt_lib_masks_t *lm = self->data;
  GtkTreeView *treeview = GTK_TREE_VIEW(lm->treeview);
  GtkTreePath *path = NULL;
  if(dt_is_valid_maskid(lm->copy_drag_formid))
    _copy_drag_target(treeview,
                      (gint)(lm->copy_drag_start_x + offset_x),
                      (gint)(lm->copy_drag_start_y + offset_y),
                      &(dt_mask_id_t){ NO_MASKID }, &path);
  gtk_tree_view_set_drag_dest_row(treeview, path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
  if(path) gtk_tree_path_free(path);
}

static void _tree_copy_drag_end(GtkGestureDrag *gesture, double offset_x,
                                double offset_y, dt_lib_module_t *self)
{
  (void)gesture;
  dt_lib_masks_t *lm = self->data;
  GtkTreeView *treeview = GTK_TREE_VIEW(lm->treeview);
  gtk_tree_view_set_drag_dest_row(treeview, NULL, GTK_TREE_VIEW_DROP_BEFORE);
  const dt_mask_id_t source_groupid = lm->copy_drag_groupid;
  const dt_mask_id_t formid = lm->copy_drag_formid;
  const double x = lm->copy_drag_start_x;
  const double y = lm->copy_drag_start_y;
  lm->copy_drag_groupid = lm->copy_drag_formid = NO_MASKID;

  if(!dt_is_valid_maskid(formid)
     || !gtk_drag_check_threshold(lm->treeview, (gint)x, (gint)y,
                                  (gint)(x + offset_x), (gint)(y + offset_y))
     || !_copy_drag_group_owner(source_groupid))
    return;

  dt_masks_point_group_t *source = _copy_drag_source_point(source_groupid, formid);
  dt_mask_id_t target_groupid = NO_MASKID;
  dt_iop_module_t *owner = _copy_drag_target(treeview, (gint)(x + offset_x),
                                             (gint)(y + offset_y), &target_groupid,
                                             NULL);
  if(!source || !owner) return;

  dt_masks_point_group_t *copy_point = malloc(sizeof(*copy_point));
  if(!copy_point) return;
  const int state = source->state;
  const float opacity = source->opacity;
  const gboolean group_undo = darktable.undo && darktable.undo->group == DT_UNDO_NONE;
  if(group_undo) dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  const dt_mask_id_t copyid = dt_masks_form_duplicate(darktable.develop, formid);
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);
  dt_masks_form_t *target = dt_masks_get_from_id(darktable.develop, target_groupid);
  const dt_masks_form_t *copy = dt_masks_get_from_id(darktable.develop, copyid);
  gboolean added = FALSE;
  if(target && target->type == DT_MASKS_GROUP && copy
     && !(copy->type & DT_MASKS_GROUP))
  {
    copy_point->formid = copyid;
    copy_point->parentid = target_groupid;
    copy_point->state = state;
    if(!(copy_point->state & DT_MASKS_STATE_OP))
      copy_point->state |= DT_MASKS_STATE_UNION;
    copy_point->opacity = opacity;
    target->points = g_list_append(target->points, copy_point);
    added = TRUE;
  }
  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
  if(!added) free(copy_point);
  if(added) dt_dev_add_masks_history_item(darktable.develop, owner, TRUE);
  if(group_undo) dt_undo_end_group(darktable.undo);
  if(added)
  {
    dt_masks_iop_update(owner);
    dt_dev_masks_selection_change(darktable.develop, owner, copyid);
  }
}

static void _tree_copy_drag_cancel(GtkGesture *gesture, GdkEventSequence *sequence,
                                   dt_lib_module_t *self)
{
  (void)gesture;
  (void)sequence;
  dt_lib_masks_t *lm = self->data;
  lm->copy_drag_groupid = lm->copy_drag_formid = NO_MASKID;
  gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(lm->treeview), NULL, GTK_TREE_VIEW_DROP_BEFORE);
}

static void _tree_invert_mask(GtkMenuItem *item, dt_lib_module_t *self)
{
  (void)item;
  dt_lib_masks_t *lm = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  dt_mask_id_t groupid = NO_MASKID;
  dt_iop_module_t *owner = _selected_localmask_group_owner(model, items, &groupid);
  if(!owner)
  {
    owner = _selected_group_owner(model, items, &groupid);
    if(!owner || !_standard_group_is_exclusive(owner, groupid)
       || (owner->blend_params->mask_mode & DEVELOP_MASK_CONDITIONAL)
       || (darktable.develop->form_gui && darktable.develop->form_gui->creation))
      owner = NULL;
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  const dt_masks_form_t *group = dt_masks_get_from_id(darktable.develop, groupid);
  if(!owner || !group || !group->points) return;

  dt_develop_blend_params_t bp = *owner->blend_params;
  bp.mask_combine ^= DEVELOP_COMBINE_MASKS_POS;
  dt_iop_commit_blend_params(owner, &bp, NULL);
  dt_dev_add_masks_history_item(darktable.develop, owner, TRUE);
  dt_iop_gui_update_blending(owner);
}

static void _tree_duplicate_mask(GtkMenuItem *item, dt_lib_module_t *self)
{
  dt_develop_t *dev = darktable.develop;
  dt_lib_masks_t *lm = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  dt_mask_id_t groupid = NO_MASKID;
  dt_iop_module_t *owner = _selected_localmask_group_owner(model, items, &groupid);
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  const dt_masks_form_t *group = dt_masks_get_from_id(dev, groupid);
  if(!owner || !group || !group->points || !dev->proxy.localadjust.module
     || !dev->proxy.localadjust.duplicate_mask)
    return;

  const gboolean invert = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "invert-copy"));
  dt_iop_module_t *copy =
    dev->proxy.localadjust.duplicate_mask(dev->proxy.localadjust.module, owner, invert);
  if(copy && copy->blend_params)
    dt_dev_masks_selection_change(dev, copy, copy->blend_params->mask_id);
}

static void _tree_duplicate_component(GtkMenuItem *item, dt_lib_module_t *self)
{
  (void)item;
  dt_lib_masks_t *lm = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  dt_masks_form_t *group = NULL;
  dt_masks_point_group_t *point = NULL;
  dt_mask_id_t formid = NO_MASKID;
  dt_iop_module_t *owner = _selected_localmask_component(model, items,
                                                          &group, &point, &formid);
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  if(!owner) return;

  const dt_mask_id_t groupid = group->formid;
  const uint32_t state = point->state;
  const float opacity = point->opacity;
  dt_masks_point_group_t *copy_point = malloc(sizeof(*copy_point));
  if(!copy_point) return;
  const gboolean group_undo = darktable.undo && darktable.undo->group == DT_UNDO_NONE;
  if(group_undo) dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);
  const dt_mask_id_t copyid = dt_masks_form_duplicate(darktable.develop, formid);
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);
  group = dt_masks_get_from_id(darktable.develop, groupid);
  const dt_masks_form_t *copy = dt_masks_get_from_id(darktable.develop, copyid);
  gboolean added = FALSE;
  if(group && (group->type & DT_MASKS_GROUP) && copy
     && !(copy->type & DT_MASKS_GROUP))
  {
    copy_point->formid = copyid;
    copy_point->parentid = groupid;
    copy_point->state = state | DT_MASKS_STATE_SHOW;
    if(!(copy_point->state & DT_MASKS_STATE_OP))
      copy_point->state |= DT_MASKS_STATE_UNION;
    copy_point->opacity = opacity;
    group->points = g_list_append(group->points, copy_point);
    added = TRUE;
  }
  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
  if(!added) free(copy_point);
  if(added) dt_dev_add_masks_history_item(darktable.develop, owner, TRUE);
  if(group_undo) dt_undo_end_group(darktable.undo);
  if(added)
  {
    dt_masks_iop_update(owner);
    dt_dev_masks_selection_change(darktable.develop, owner, copyid);
  }
}

static gboolean _select_local_action_row(dt_lib_module_t *self,
                                         dt_iop_module_t *owner,
                                         const dt_mask_id_t childid)
{
  dt_develop_t *dev = darktable.develop;
  dt_lib_masks_t *lm = self ? self->data : NULL;
  if(!dev || !dev->gui_attached || !lm || !lm->treeview || !owner
     || !g_list_find(dev->iop, owner) || !dt_iop_module_is(owner, "localmask")
     || !owner->blend_params) return FALSE;

  // The Advanced panel can stay collapsed while Local Adjustments changes
  // masks. Its queued redraw has not necessarily rebuilt the tree yet, but
  // actions below need to select a row from the current model immediately.
  dt_lib_gui_queue_update(self);
  dt_lib_gui_update(self);

  const dt_mask_id_t rootid = owner->blend_params->mask_id;
  const dt_masks_form_t *group = dt_masks_get_from_id(dev, rootid);
  if(!group || group->type != DT_MASKS_GROUP) return FALSE;
  if(dt_is_valid_maskid(childid))
  {
    guint matches = 0;
    for(const GList *it = group->points; it; it = g_list_next(it))
    {
      const dt_masks_point_group_t *point = it->data;
      if(point && point->formid == childid) matches++;
    }
    if(matches != 1) return FALSE;
  }

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeIter root;
  if(!model || !gtk_tree_model_get_iter_first(model, &root)) return FALSE;
  gboolean found = FALSE;
  do
  {
    dt_iop_module_t *row_owner = NULL;
    dt_mask_id_t row_groupid = NO_MASKID;
    dt_mask_id_t row_id = NO_MASKID;
    _lib_masks_get_values(model, &root, &row_owner, &row_groupid, &row_id);
    if(row_owner == owner && row_groupid == NO_MASKID && row_id == rootid)
    {
      found = TRUE;
      break;
    }
  } while(gtk_tree_model_iter_next(model, &root));
  if(!found) return FALSE;

  GtkTreeIter target = root;
  if(dt_is_valid_maskid(childid))
  {
    if(!gtk_tree_model_iter_children(model, &target, &root)) return FALSE;
    found = FALSE;
    do
    {
      dt_iop_module_t *row_owner = NULL;
      dt_mask_id_t row_groupid = NO_MASKID;
      dt_mask_id_t row_id = NO_MASKID;
      _lib_masks_get_values(model, &target, &row_owner, &row_groupid, &row_id);
      if(row_owner == owner && row_groupid == rootid && row_id == childid)
      {
        found = TRUE;
        break;
      }
    } while(gtk_tree_model_iter_next(model, &target));
    if(!found) return FALSE;
  }

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  DT_ENTER_GUI_UPDATE();
  gtk_tree_selection_unselect_all(selection);
  GtkTreePath *path = gtk_tree_model_get_path(model, &target);
  gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
  gtk_tree_selection_select_iter(selection, &target);
  gtk_tree_path_free(path);
  dev->mask_form_selected_id = dt_is_valid_maskid(childid) ? childid : NO_MASKID;
  DT_LEAVE_GUI_UPDATE();
  _update_all_properties(lm);
  _overlay_set_selected(selection);
  return TRUE;
}

static dt_masks_point_group_t *_local_group_point(const dt_mask_id_t groupid,
                                                   const dt_mask_id_t childid)
{
  const dt_masks_form_t *group = dt_masks_get_from_id(darktable.develop, groupid);
  if(!group || group->type != DT_MASKS_GROUP) return NULL;
  for(const GList *it = group->points; it; it = g_list_next(it))
  {
    dt_masks_point_group_t *point = it->data;
    if(point && point->formid == childid) return point;
  }
  return NULL;
}

static gboolean _local_action(dt_lib_module_t *self,
                              dt_iop_module_t *owner,
                              const dt_mask_id_t childid,
                              const dt_dev_mask_ui_action_t action,
                              const int shape_type)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !darktable.undo
     || darktable.undo->group != DT_UNDO_NONE
     || (dev->form_gui && dev->form_gui->creation)) return FALSE;

  const gboolean child_action = action == DT_DEV_MASK_UI_INVERT_COMPONENT
                                || action == DT_DEV_MASK_UI_DUPLICATE_COMPONENT
                                || action == DT_DEV_MASK_UI_MOVE_COMPONENT_TO_NEW_MASK
                                || action == DT_DEV_MASK_UI_DELETE_COMPONENT;
  const gboolean shape_action = action == DT_DEV_MASK_UI_ADD_COMPONENT
                                || action == DT_DEV_MASK_UI_SUBTRACT_COMPONENT;
  if(child_action != dt_is_valid_maskid(childid) || (shape_action &&
     !(shape_type == DT_MASKS_BRUSH || shape_type == DT_MASKS_ELLIPSE
       || shape_type == DT_MASKS_PATH || shape_type == DT_MASKS_GRADIENT)))
    return FALSE;

  // A newly committed shape can precede the Mask Manager tree refresh. Shape
  // creation needs the live owner and group, not a selection in that tree.
  if(shape_action)
  {
    if(self != dev->proxy.masks.module || !owner || !g_list_find(dev->iop, owner)
       || !dt_iop_module_is(owner, "localmask") || owner->multi_priority <= 0
       || !owner->blend_params || !dev->proxy.localadjust.module
       || !dev->proxy.localadjust.select_mask
       || !dev->proxy.localadjust.selected_mask) return FALSE;
    const dt_mask_id_t rootid = owner->blend_params->mask_id;
    if(rootid == NO_MASKID)
    {
      if(action != DT_DEV_MASK_UI_ADD_COMPONENT) return FALSE;
    }
    else
    {
      const dt_masks_form_t *group = dt_masks_get_from_id(dev, rootid);
      if(!group || group->type != DT_MASKS_GROUP) return FALSE;
      for(const GList *it = dev->iop; it; it = g_list_next(it))
      {
        const dt_iop_module_t *other = it->data;
        if(other != owner && other->blend_params
           && other->blend_params->mask_id == rootid) return FALSE;
      }
    }
    dev->proxy.localadjust.select_mask(dev->proxy.localadjust.module, owner);
    if(dev->proxy.localadjust.selected_mask(dev->proxy.localadjust.module) != owner
       || owner->blend_params->mask_id != rootid) return FALSE;
    const gboolean armed = rootid == NO_MASKID
      ? _tree_add_shape(NULL, GINT_TO_POINTER(shape_type))
      : _tree_add_shape_to_target(rootid, TRUE,
                                  action == DT_DEV_MASK_UI_SUBTRACT_COMPONENT,
                                  owner, (dt_masks_type_t)shape_type);
    if(!armed) return FALSE;
    if(!dev->form_gui || !dev->form_gui->creation
       || dev->form_gui->creation_module != owner)
    {
      if(dev->form_gui && dev->form_gui->creation) dt_masks_cancel_creation();
      return FALSE;
    }
    return TRUE;
  }
  if(!_select_local_action_row(self, owner, childid)) return FALSE;

  dt_lib_masks_t *lm = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  const gboolean valid = child_action
    ? _selected_localmask_component(model, items, NULL, NULL, NULL) == owner
    : _selected_localmask_group_owner(model, items, NULL) == owner;
  gboolean safe_move_or_delete = TRUE;
  if(valid && (action == DT_DEV_MASK_UI_MOVE_COMPONENT_TO_NEW_MASK
               || action == DT_DEV_MASK_UI_DELETE_COMPONENT))
  {
    dt_lib_masks_shape_context_t context;
    safe_move_or_delete = _shape_context(model, items->data, &context)
                          && context.owner == owner
                          && context.formid == childid
                          && context.groupid == owner->blend_params->mask_id;
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  if(!valid || !safe_move_or_delete) return FALSE;

  switch(action)
  {
    case DT_DEV_MASK_UI_INVERT_PARENT:
    {
      const dt_masks_form_t *group = dt_masks_get_from_id(dev, owner->blend_params->mask_id);
      if(!group || !group->points) return FALSE;
      const int before = owner->blend_params->mask_combine;
      _tree_invert_mask(NULL, self);
      return owner->blend_params->mask_combine != before;
    }
    case DT_DEV_MASK_UI_DUPLICATE_PARENT:
    {
      if(!dev->proxy.localadjust.module || !dev->proxy.localadjust.duplicate_mask)
        return FALSE;
      dt_iop_module_t *copy = dev->proxy.localadjust.duplicate_mask(
        dev->proxy.localadjust.module, owner, FALSE);
      if(copy && copy->blend_params)
        dt_dev_masks_selection_change(dev, copy, copy->blend_params->mask_id);
      return copy != NULL;
    }
    case DT_DEV_MASK_UI_INVERT_COMPONENT:
    {
      dt_masks_point_group_t *point = _local_group_point(owner->blend_params->mask_id, childid);
      if(!point) return FALSE;
      const dt_masks_state_t before = point->state;
      _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_INVERSE));
      return point->state != before;
    }
    case DT_DEV_MASK_UI_DUPLICATE_COMPONENT:
    {
      const guint before = g_list_length(dev->forms);
      _tree_duplicate_component(NULL, self);
      return g_list_length(dev->forms) > before;
    }
    case DT_DEV_MASK_UI_MOVE_COMPONENT_TO_NEW_MASK:
      _tree_create_mask_from_shape(NULL, self);
      return _local_group_point(owner->blend_params->mask_id, childid) == NULL;
    case DT_DEV_MASK_UI_DELETE_COMPONENT:
      _tree_delete_component(NULL, self);
      return _local_group_point(owner->blend_params->mask_id, childid) == NULL;
    default:
      return FALSE;
  }
}

static void _overlay_row_context(GtkTreeModel *model, GtkTreeIter *iter,
                                 dt_mask_id_t *id, dt_mask_id_t *groupid,
                                 uint32_t *state, dt_iop_module_t **owner)
{
  *id = *groupid = NO_MASKID;
  *state = DT_MASKS_STATE_NONE;
  *owner = NULL;
  _lib_masks_get_values(model, iter, owner, groupid, id);
  const dt_masks_form_t *group = dt_masks_get_from_id(darktable.develop, *groupid);
  if(!group || !(group->type & DT_MASKS_GROUP))
  {
    *groupid = NO_MASKID;
    return;
  }
  for(const GList *points = group->points; points; points = g_list_next(points))
  {
    const dt_masks_point_group_t *point = points->data;
    if(point->formid == *id)
    {
      *state = point->state;
      return;
    }
  }
  *groupid = NO_MASKID;
}

static void _overlay_set_selected(GtkTreeSelection *selection)
{
  dt_develop_t *dev = darktable.develop;
  dt_mask_id_t id = NO_MASKID;
  dt_mask_id_t groupid = NO_MASKID;
  uint32_t state = DT_MASKS_STATE_NONE;
  dt_iop_module_t *owner = NULL;
  if(gtk_tree_selection_count_selected_rows(selection) == 1)
  {
    GtkTreeModel *model = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
    GtkTreeIter iter;
    if(rows && gtk_tree_model_get_iter(model, &iter, rows->data))
      _overlay_row_context(model, &iter, &id, &groupid, &state, &owner);
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  }
  if(dev->mask_overlay_imgid != dev->image_storage.id)
  {
    dev->mask_overlay_hover_id = NO_MASKID;
    dev->mask_overlay_hover_group_id = NO_MASKID;
    dev->mask_overlay_hover_state = DT_MASKS_STATE_NONE;
    dev->mask_overlay_hover_owner = NULL;
  }
  if(dev->mask_overlay_selected_id != id || dev->mask_overlay_selected_group_id != groupid
     || dev->mask_overlay_selected_state != state || dev->mask_overlay_selected_owner != owner
     || dev->mask_overlay_imgid != dev->image_storage.id)
  {
    dev->mask_overlay_selected_id = id;
    dev->mask_overlay_selected_group_id = groupid;
    dev->mask_overlay_selected_state = state;
    dev->mask_overlay_selected_owner = owner;
    dev->mask_overlay_imgid = dev->image_storage.id;
    dt_control_queue_redraw_center();
  }
}

static gboolean _tree_overlay_motion(GtkWidget *treeview, GdkEventMotion *event, gpointer user_data)
{
  (void)user_data;
  dt_mask_id_t id = NO_MASKID;
  dt_mask_id_t groupid = NO_MASKID;
  uint32_t state = DT_MASKS_STATE_NONE;
  dt_iop_module_t *owner = NULL;
  GtkTreePath *path = NULL;
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), event->x, event->y,
                                   &path, NULL, NULL, NULL))
  {
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
    if(gtk_tree_model_get_iter(model, &iter, path))
      _overlay_row_context(model, &iter, &id, &groupid, &state, &owner);
    gtk_tree_path_free(path);
  }
  dt_develop_t *dev = darktable.develop;
  if(dev->mask_overlay_hover_id != id || dev->mask_overlay_hover_group_id != groupid
     || dev->mask_overlay_hover_state != state || dev->mask_overlay_hover_owner != owner
     || dev->mask_overlay_imgid != dev->image_storage.id)
  {
    dev->mask_overlay_hover_id = id;
    dev->mask_overlay_hover_group_id = groupid;
    dev->mask_overlay_hover_state = state;
    dev->mask_overlay_hover_owner = owner;
    dev->mask_overlay_imgid = dev->image_storage.id;
    dt_control_queue_redraw_center();
  }
  return FALSE;
}

static gboolean _tree_overlay_leave(GtkWidget *treeview, GdkEventCrossing *event, gpointer user_data)
{
  (void)treeview;
  (void)event;
  (void)user_data;
  dt_develop_t *dev = darktable.develop;
  if(dt_is_valid_maskid(dev->mask_overlay_hover_id))
  {
    dev->mask_overlay_hover_id = NO_MASKID;
    dev->mask_overlay_hover_group_id = NO_MASKID;
    dev->mask_overlay_hover_state = DT_MASKS_STATE_NONE;
    dev->mask_overlay_hover_owner = NULL;
    dt_control_queue_redraw_center();
  }
  return FALSE;
}

static void _show_overlay_toggled(GtkToggleButton *button, gpointer user_data)
{
  (void)user_data;
  DT_GUARD_GUI_UPDATE();
  darktable.develop->mask_overlay_show = gtk_toggle_button_get_active(button);
  dt_control_queue_redraw_center();
  if(darktable.develop->proxy.localadjust.module)
    dt_lib_gui_queue_update(darktable.develop->proxy.localadjust.module);
}

static void _all_shapes_toggled(GtkToggleButton *button, dt_lib_module_t *self)
{
  (void)button;
  dt_lib_masks_t *lm = self->data;
  if(!lm) return;
  lm->tree_hash_valid = FALSE;
  _lib_masks_recreate_list(self);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_lib_masks_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_masks_brush_arm_new_component();
  // we reset all "show mask" icon of iops
  dt_masks_reset_show_masks_icons();

  int nb = gtk_tree_selection_count_selected_rows(selection);
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(self->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  dt_iop_module_t *owner = _selection_owner(model, items);
  if(nb > 0)
  {
    GList *rows = NULL;
    for(const GList *it = items; it; it = g_list_next(it))
      rows = g_list_append(rows, gtk_tree_row_reference_new(model, it->data));

    dt_develop_t *dev = darktable.develop;
    if(dev->proxy.localadjust.module)
    {
      dt_mask_id_t childid = NO_MASKID;
      gboolean local_row = FALSE;
      if(nb == 1 && owner && dt_iop_module_is(owner, "localmask")
         && owner->blend_params)
      {
        GtkTreeIter iter;
        if(gtk_tree_model_get_iter(model, &iter, items->data))
        {
          dt_mask_id_t groupid = NO_MASKID;
          dt_mask_id_t formid = NO_MASKID;
          _lib_masks_get_values(model, &iter, NULL, &groupid, &formid);
          if(groupid == NO_MASKID && formid == owner->blend_params->mask_id)
            local_row = TRUE;
          else if(groupid == owner->blend_params->mask_id
                  && _selected_localmask_component(model, items, NULL, NULL, NULL) == owner)
          {
            childid = formid;
            local_row = TRUE;
          }
        }
      }
      if(local_row && dev->proxy.localadjust.select_component)
        dev->proxy.localadjust.select_component(dev->proxy.localadjust.module, owner, childid);
      else if(dev->proxy.localadjust.select_mask)
        dev->proxy.localadjust.select_mask(dev->proxy.localadjust.module, owner);
    }

    // focus clears the mask overlay and selection; restore the rows afterwards
    // disabled owners stay disabled and are edited through the mask manager
    dt_iop_request_focus(owner && owner->enabled ? owner : NULL);
    DT_ENTER_GUI_UPDATE();
    gtk_tree_selection_unselect_all(selection);
    for(const GList *it = rows; it; it = g_list_next(it))
    {
      GtkTreePath *path = gtk_tree_row_reference_get_path(it->data);
      if(path)
      {
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(self->treeview), path);
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_path_free(path);
      }
    }
    DT_LEAVE_GUI_UPDATE();
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_row_reference_free);
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
    items = gtk_tree_selection_get_selected_rows(selection, NULL);
    nb = gtk_tree_selection_count_selected_rows(selection);
  }

  // create the visible group only after the owning module has canvas focus
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);

  for(const GList *items_iter = items;
      items_iter;
      items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;

    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      dt_mask_id_t grid = INVALID_MASKID;
      dt_mask_id_t id = INVALID_MASKID;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
      if(form)
      {
        dt_masks_point_group_t *fpt = malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = id;
        fpt->parentid = grid;
        fpt->state = DT_MASKS_STATE_USE;
        fpt->opacity = 1.0f;
        grp->points = g_list_append(grp->points, fpt);
        // we eventually set the "show masks" icon of iops
        if(nb == 1 && (form->type & DT_MASKS_GROUP))
        {
          dt_iop_module_t *module = owner;

          if(module && module->enabled && module->blend_data
             && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
             && !(module->flags() & IOP_FLAGS_NO_MASKS))
          {
            dt_iop_gui_blend_data_t *bd = module->blend_data;
            bd->masks_shown = DT_MASKS_EDIT_FULL;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
            gtk_widget_queue_draw(bd->masks_edit);
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  dt_masks_form_t *grp2 = dt_masks_create(DT_MASKS_GROUP);
  grp2->formid = NO_MASKID;
  dt_masks_group_ungroup(grp2, grp);

  // don't call dt_masks_change_form_gui because it triggers a selection change again
  dt_masks_clear_form_gui(darktable.develop);
  darktable.develop->form_visible = grp2;

  // update sticky accels window
  if(darktable.view_manager->accels_window.window
     && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  darktable.develop->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  dt_control_queue_redraw_center();

  _update_all_properties(self);
  _overlay_set_selected(selection);
}

static void _tree_button_pressed_cb(GtkGestureSingle *gesture, int n_press, double x, double y, dt_lib_module_t *self)
{
  GtkWidget *treeview = dt_gui_get_widget(gesture);
  // we first need to adjust selection
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));

  GtkTreePath *mouse_path = NULL;
  GtkTreeIter iter;
  dt_iop_module_t *module = NULL;
  gboolean on_row = FALSE;
  gboolean clicked_group = FALSE;
  dt_mask_id_t clicked_groupid = NO_MASKID;
  dt_iop_module_t *clicked_group_owner = NULL;
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview),
                                   (gint)x, (gint)y, &mouse_path, NULL,
                                   NULL, NULL))
  {
    on_row = TRUE;
    // we retrieve the iter and module from path
    if(gtk_tree_model_get_iter(model, &iter, mouse_path))
    {
      _lib_masks_get_values(model, &iter, &module, NULL, NULL);
      if(gtk_tree_path_get_depth(mouse_path) == 1)
      {
        dt_mask_id_t id = NO_MASKID;
        _lib_masks_get_values(model, &iter, NULL, NULL, &id);
        const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
        clicked_group = form && form->type == DT_MASKS_GROUP;
        if(clicked_group)
        {
          GList row = { .data = mouse_path };
          clicked_group_owner = _selected_group_owner(model, &row, &clicked_groupid);
        }
      }
    }
  }
  /* single click with the right mouse button? */
  const guint button = gtk_gesture_single_get_current_button(gesture);
  if(button == GDK_BUTTON_PRIMARY)
  {
    // if click on a blank space, then deselect all
    if(!on_row)
    {
      gtk_tree_selection_unselect_all(selection);
    }
  }
  else if(button == GDK_BUTTON_SECONDARY)
  {
    const GdkModifierType state =
      dt_gui_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    // if we are already inside the selection, no change
    if(on_row
       && !gtk_tree_selection_path_is_selected(selection, mouse_path))
    {
      if(!dt_modifier_is(state, GDK_CONTROL_MASK))
        gtk_tree_selection_unselect_all(selection);

      gtk_tree_selection_select_path(selection, mouse_path);
      gtk_tree_path_free(mouse_path);
    }

    // and we display the context-menu
    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
    GtkWidget *item;
    gboolean has_menu_items = FALSE;

    // we get all infos from selection
    const int nb = gtk_tree_selection_count_selected_rows(selection);
    gboolean from_group = FALSE;

    gboolean is_first_row = FALSE;
    gboolean is_last_row = FALSE;
    dt_masks_state_t selected_states = DT_MASKS_STATE_NONE;

    int grpid = NO_MASKID;
    int depth = 0;
    dt_masks_form_t *grp = NULL;

    if(nb > 0)
    {
      GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
      GtkTreePath *it0 = (GtkTreePath *)selected->data;
      depth = gtk_tree_path_get_depth(it0);
      if(nb == 1)
      {
        // before freeing the list of selected rows, we check if the
        // form is a group or not
        if(gtk_tree_model_get_iter(model, &iter, it0))
        {
          _lib_masks_get_values(model, &iter, NULL, NULL, &grpid);
          grp = dt_masks_get_from_id(darktable.develop, grpid);
        }

        // if depth > 1 then check if the selected item is the first
        // or last in the group. This is used to enable/disable some
        // feature only meaningful for rows with prev/next.

        GtkTreeIter it;
        GtkTreePath *item = gtk_tree_path_copy(it0);
        gtk_tree_model_get_iter(model, &it, item);
        is_last_row = !gtk_tree_model_iter_next(model, &it);

        if(!is_last_row && !gtk_tree_path_prev(item))
        {
          is_first_row = TRUE;
        }
        gtk_tree_path_free(item);
      }

      for(const GList *items_iter = selected;
          items_iter;
          items_iter = g_list_next(items_iter))
      {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;

        if(gtk_tree_model_get_iter(model, &iter, item))
        {
          dt_mask_id_t grid = INVALID_MASKID;
          dt_mask_id_t id = INVALID_MASKID;
          _lib_masks_get_values(model, &iter, NULL, &grid, &id);

          dt_masks_form_t *grp2 = dt_masks_get_from_id(darktable.develop, grid);
          if(grp2 && (grp2->type & DT_MASKS_GROUP))
          {
            for(const GList *pts = grp2->points; pts; pts = g_list_next(pts))
            {
              dt_masks_point_group_t *pt = pts->data;
              if(pt->formid == id) selected_states |= pt->state;
            }
          }
        }
      }

      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }

    if(depth > 1)
      from_group = TRUE;

    dt_iop_module_t *add_owner = NULL;
    dt_iop_module_t *group_owner = NULL;
    dt_iop_module_t *standard_owner = NULL;
    dt_iop_module_t *component_owner = NULL;
    if(nb == 1)
    {
      GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
      add_owner = _selected_group_owner(model, selected, NULL);
      if(add_owner && dt_iop_module_is(add_owner, "localmask"))
        group_owner = add_owner;
      else if(_standard_group_is_exclusive(add_owner, grpid)
              && !(darktable.develop->form_gui && darktable.develop->form_gui->creation))
        standard_owner = add_owner;
      component_owner = _selected_localmask_component(model, selected, NULL, NULL, NULL);
      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }

    if(clicked_group || (!on_row && nb == 0))
    {
      const gboolean can_add = !clicked_group
        || (clicked_group_owner
            && (dt_iop_module_is(clicked_group_owner, "localmask")
                || _can_add_to_standard_group(clicked_group_owner)));
      item = gtk_menu_item_new_with_label(clicked_group
                                            ? _("add brush to mask") : _("new brush shape"));
      gtk_widget_set_sensitive(item, can_add);
      if(clicked_group)
      {
        g_object_set_data(G_OBJECT(item), "group-context", GINT_TO_POINTER(TRUE));
        g_object_set_data(G_OBJECT(item), "target-group-id", GINT_TO_POINTER(clicked_groupid));
        g_object_set_data(G_OBJECT(item), "target-owner", clicked_group_owner);
      }
      g_signal_connect(item, "activate", G_CALLBACK(_tree_add_shape_menu),
                       GINT_TO_POINTER(DT_MASKS_BRUSH));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(clicked_group
                                            ? _("add radial gradient to mask")
                                            : _("new radial gradient shape"));
      gtk_widget_set_sensitive(item, can_add);
      if(clicked_group)
      {
        g_object_set_data(G_OBJECT(item), "group-context", GINT_TO_POINTER(TRUE));
        g_object_set_data(G_OBJECT(item), "target-group-id", GINT_TO_POINTER(clicked_groupid));
        g_object_set_data(G_OBJECT(item), "target-owner", clicked_group_owner);
      }
      g_signal_connect(item, "activate", G_CALLBACK(_tree_add_shape_menu),
                       GINT_TO_POINTER(DT_MASKS_ELLIPSE));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(clicked_group
                                            ? _("add path to mask") : _("new path shape"));
      gtk_widget_set_sensitive(item, can_add);
      if(clicked_group)
      {
        g_object_set_data(G_OBJECT(item), "group-context", GINT_TO_POINTER(TRUE));
        g_object_set_data(G_OBJECT(item), "target-group-id", GINT_TO_POINTER(clicked_groupid));
        g_object_set_data(G_OBJECT(item), "target-owner", clicked_group_owner);
      }
      g_signal_connect(item, "activate", G_CALLBACK(_tree_add_shape_menu),
                       GINT_TO_POINTER(DT_MASKS_PATH));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(clicked_group
                                            ? _("add gradient to mask")
                                            : _("new gradient shape"));
      gtk_widget_set_sensitive(item, can_add);
      if(clicked_group)
      {
        g_object_set_data(G_OBJECT(item), "group-context", GINT_TO_POINTER(TRUE));
        g_object_set_data(G_OBJECT(item), "target-group-id", GINT_TO_POINTER(clicked_groupid));
        g_object_set_data(G_OBJECT(item), "target-owner", clicked_group_owner);
      }
      g_signal_connect(item, "activate", G_CALLBACK(_tree_add_shape_menu),
                       GINT_TO_POINTER(DT_MASKS_GRADIENT));
      gtk_menu_shell_append(menu, item);
      has_menu_items = TRUE;
    }

    if(grp && grp->type & DT_MASKS_GROUP)
    {
      if(group_owner) module = group_owner;
      // existing forms
      gboolean has_unused_shapes = FALSE;
      GtkWidget *menu0 = gtk_menu_new();

      for(GList *forms = darktable.develop->forms;
          forms;
          forms = g_list_next(forms))
      {
        dt_masks_form_t *form = forms->data;
        if((form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) || form->formid == grpid)
        {
          continue;
        }
        char str[10000] = "";
        g_strlcat(str, form->name, sizeof(str));
        int nbuse = 0;

        // we search were this form is used
        for(const GList *modules = darktable.develop->iop;
            modules;
            modules = g_list_next(modules))
        {
          dt_iop_module_t *m = modules->data;
          dt_masks_form_t *grp = dt_masks_get_from_id(m->dev, m->blend_params->mask_id);
          if(grp && (grp->type & DT_MASKS_GROUP))
          {
            for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
            {
              dt_masks_point_group_t *pt = pts->data;
              if(pt->formid == form->formid)
              {
                if(m == module)
                {
                  nbuse = -1;
                  break;
                }
                if(nbuse == 0) g_strlcat(str, " (", sizeof(str));
                g_strlcat(str, " ", sizeof(str));
                gchar *module_label = dt_history_item_get_name(m);
                g_strlcat(str, module_label, sizeof(str));
                g_free(module_label);
                nbuse++;
              }
            }
          }
        }
        if(nbuse != -1)
        {
          if(nbuse > 0) g_strlcat(str, " )", sizeof(str));

          // we add the menu entry
          item = gtk_menu_item_new_with_label(str);
          g_object_set_data(G_OBJECT(item), "formid", GUINT_TO_POINTER(form->formid));
          g_object_set_data(G_OBJECT(item), "module", module);
          g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_tree_add_exist), grp);
          gtk_menu_shell_append(GTK_MENU_SHELL(menu0), item);
          has_unused_shapes = TRUE;
        }
      }

      if(has_unused_shapes)
      {
        item = gtk_menu_item_new_with_label(_("add existing shape to mask"));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu0);
        gtk_menu_shell_append(menu, item);
        has_menu_items = TRUE;
      }
    }

    if((group_owner || standard_owner) && grp->points)
    {
      dt_iop_module_t *mask_owner = group_owner ? group_owner : standard_owner;
      gboolean has_mask_edits = FALSE;
      if(!(mask_owner->blend_params->mask_combine & DEVELOP_COMBINE_MASKS_POS))
      {
        if(has_menu_items)
          gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        GtkWidget *subtract_menu = gtk_menu_new();
        _tree_append_subtract_tool(subtract_menu, _("brush"), DT_MASKS_BRUSH, grpid, mask_owner);
        _tree_append_subtract_tool(subtract_menu, _("radial gradient"), DT_MASKS_ELLIPSE, grpid,
                                   mask_owner);
        _tree_append_subtract_tool(subtract_menu, _("path"), DT_MASKS_PATH, grpid, mask_owner);
        _tree_append_subtract_tool(subtract_menu, _("gradient"), DT_MASKS_GRADIENT, grpid,
                                   mask_owner);
        item = gtk_menu_item_new_with_label(_("subtract from mask"));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), subtract_menu);
        gtk_menu_shell_append(menu, item);
        has_menu_items = has_mask_edits = TRUE;
      }

      if(group_owner || !(standard_owner->blend_params->mask_mode & DEVELOP_MASK_CONDITIONAL))
      {
        if(has_menu_items && !has_mask_edits)
          gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        item = gtk_check_menu_item_new_with_label(_("invert mask"));
        gtk_widget_set_tooltip_text(item,
                                    _("when checked, adjustments affect the area outside the mask"));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                       mask_owner->blend_params->mask_combine
                                         & DEVELOP_COMBINE_MASKS_POS);
        g_signal_connect(item, "activate", G_CALLBACK(_tree_invert_mask), self);
        gtk_menu_shell_append(menu, item);
        has_menu_items = has_mask_edits = TRUE;
      }

      if(group_owner)
      {
        if(has_menu_items)
          gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        item = gtk_menu_item_new_with_label(_("duplicate mask"));
        g_signal_connect(item, "activate", G_CALLBACK(_tree_duplicate_mask), self);
        gtk_menu_shell_append(menu, item);

        item = gtk_menu_item_new_with_label(_("duplicate and invert mask"));
        g_object_set_data(G_OBJECT(item), "invert-copy", GINT_TO_POINTER(TRUE));
        g_signal_connect(item, "activate", G_CALLBACK(_tree_duplicate_mask), self);
        gtk_menu_shell_append(menu, item);
        has_menu_items = TRUE;
      }
    }

    if(!from_group && nb > 0)
    {
      if(has_menu_items)
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grpid);
      if(!(grp && (grp->type & DT_MASKS_GROUP)))
      {
        if(nb == 1)
        {
          item = gtk_menu_item_new_with_label(_("duplicate shape"));
          g_signal_connect(item, "activate", G_CALLBACK(_tree_duplicate_shape), self);
          gtk_menu_shell_append(menu, item);
          GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
          dt_lib_masks_shape_context_t source;
          const gboolean orphan = selected && !selected->next
                                  && _shape_context(model, selected->data, &source)
                                  && !dt_is_valid_maskid(source.groupid);
          g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
          if(orphan)
          {
            item = gtk_menu_item_new_with_label(_("create new mask from shape"));
            g_signal_connect(item, "activate", G_CALLBACK(_tree_create_mask_from_shape), self);
            gtk_menu_shell_append(menu, item);
          }
          gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        }
        item = gtk_menu_item_new_with_label(nb == 1
                                              ? _("delete shape") : _("delete selected items"));
        g_signal_connect(item, "activate", G_CALLBACK(_tree_delete_shape), self);
        gtk_menu_shell_append(menu, item);
      }
      else
      {
        item = gtk_menu_item_new_with_label(_("delete group"));
        g_signal_connect(item, "activate", G_CALLBACK(_tree_delete_shape), self);
        gtk_menu_shell_append(menu, item);
      }
      has_menu_items = TRUE;
    }
    else if(nb > 0 && depth < 3)
    {
      if(has_menu_items)
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      if(component_owner)
      {
        item = gtk_menu_item_new_with_label(_("duplicate component in mask"));
        g_signal_connect(item, "activate", G_CALLBACK(_tree_duplicate_component), self);
        gtk_menu_shell_append(menu, item);
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      }
      GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
      gboolean can_delete = selected != NULL;
      gboolean can_rehome = nb == 1;
      gboolean has_localmask_child = FALSE;
      for(const GList *it = selected; it; it = g_list_next(it))
      {
        GtkTreeIter row;
        if(gtk_tree_path_get_depth(it->data) == 2
           && gtk_tree_model_get_iter(model, &row, it->data))
        {
          dt_mask_id_t row_groupid = NO_MASKID;
          _lib_masks_get_values(model, &row, NULL, &row_groupid, NULL);
          for(const GList *modules = darktable.develop->iop;
              modules; modules = g_list_next(modules))
          {
            const dt_iop_module_t *owner = modules->data;
            if(owner->blend_params && owner->blend_params->mask_id == row_groupid
               && dt_iop_module_is(owner, "localmask"))
            {
              has_localmask_child = TRUE;
              break;
            }
          }
        }
        dt_lib_masks_shape_context_t source;
        if(!_shape_context(model, it->data, &source)
           || !dt_is_valid_maskid(source.groupid))
          can_delete = can_rehome = FALSE;
      }
      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
      if(can_delete && nb == 1)
      {
        item = gtk_menu_item_new_with_label(_("create new mask from shape"));
        gtk_widget_set_sensitive(item, can_rehome);
        g_signal_connect(item, "activate", G_CALLBACK(_tree_create_mask_from_shape), self);
        gtk_menu_shell_append(menu, item);
      }
      item = gtk_menu_item_new_with_label(can_delete
                                            ? (nb == 1 ? _("delete shape")
                                                       : _("delete selected shapes"))
                                            : _("remove from group"));
      if(can_delete)
        g_signal_connect(item, "activate", G_CALLBACK(_tree_delete_component), self);
      else if(!has_localmask_child)
        g_signal_connect(item, "activate", G_CALLBACK(_tree_delete_shape), self);
      else
        gtk_widget_set_sensitive(item, FALSE);
      gtk_menu_shell_append(menu, item);
      has_menu_items = TRUE;
    }

    if(nb > 1 && !from_group)
    {
      if(has_menu_items)
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("group selected items"));
      g_signal_connect(item, "activate", G_CALLBACK(_tree_group), self);
      gtk_menu_shell_append(menu, item);
      has_menu_items = TRUE;
    }

    if(from_group && depth < 3)
    {
      if(has_menu_items)
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      _add_tree_operation(menu, _("invert shape in group"),
                          DT_MASKS_STATE_INVERSE, selected_states, TRUE);

      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      _add_tree_operation(menu, _("mode: union"),
                          DT_MASKS_STATE_UNION, selected_states, !is_last_row);
      _add_tree_operation(menu, _("mode: intersection"),
                          DT_MASKS_STATE_INTERSECTION, selected_states, !is_last_row);
      _add_tree_operation(menu, _("mode: difference"),
                          DT_MASKS_STATE_DIFFERENCE, selected_states, !is_last_row);
      _add_tree_operation(menu, _("mode: sum"),
                          DT_MASKS_STATE_SUM, selected_states, !is_last_row);
      _add_tree_operation(menu, _("mode: exclusion"),
                          DT_MASKS_STATE_EXCLUSION, selected_states, !is_last_row);

      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("move up in group"));
      gtk_widget_set_sensitive(item, !is_first_row);
      g_signal_connect(item, "activate", G_CALLBACK(_tree_moveup), self);
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(_("move down in group"));
      gtk_widget_set_sensitive(item, !is_last_row);
      g_signal_connect(item, "activate", G_CALLBACK(_tree_movedown), self);
      gtk_menu_shell_append(menu, item);
      has_menu_items = TRUE;
    }

    if(has_menu_items)
      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("delete unused shapes"));
    g_signal_connect(item, "activate", G_CALLBACK(_tree_cleanup), self);
    gtk_menu_shell_append(menu, item);

    gtk_widget_show_all(GTK_WIDGET(menu));

    const GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
  }
}

static gboolean _tree_restrict_select(GtkTreeSelection *selection,
                                      GtkTreeModel *model,
                                      GtkTreePath *path,
                                      const gboolean path_currently_selected,
                                      gpointer data)
{
  DT_GUARD_GUI_UPDATE(TRUE);

  // if the change is SELECT->UNSELECT no pb
  if(path_currently_selected) return TRUE;

  // if selection is empty, no pb
  if(gtk_tree_selection_count_selected_rows(selection) == 0) return TRUE;

  // now we unselect all members of selection with not the same parent node
  // idem for all those with a different depth
  int *indices = gtk_tree_path_get_indices(path);
  const int depth = gtk_tree_path_get_depth(path);

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  GList *items_iter = items;
  while(items_iter)
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    const int dd = gtk_tree_path_get_depth(item);
    int *ii = gtk_tree_path_get_indices(item);
    int ok = 1;
    if(dd != depth)
      ok = 0;
    else if(dd == 1)
      ok = 1;
    else if(ii[dd - 2] != indices[dd - 2])
      ok = 0;
    if(!ok)
    {
      gtk_tree_selection_unselect_path(selection, item);
      g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
      items_iter = items = gtk_tree_selection_get_selected_rows(selection, NULL);
      continue;
    }
    items_iter = g_list_next(items_iter);
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  return TRUE;
}

static gboolean _tree_query_tooltip(GtkWidget *widget,
                                    gint x,
                                    gint y,
                                    const gboolean keyboard_tip,
                                    GtkTooltip *tooltip,
                                    gpointer data)
{
  GtkTreeIter iter;
  GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
  GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
  GtkTreePath *path = NULL;
  gchar *tmp = NULL;
  gboolean show = FALSE;

  if(!gtk_tree_view_get_tooltip_context(tree_view, &x, &y,
                                        keyboard_tip, &model, &path, &iter))
    return FALSE;

  gtk_tree_model_get(model, &iter, TREE_IC_USED_VISIBLE, &show, TREE_USED_TEXT, &tmp, -1);
  if(show)
  {
    gtk_tooltip_set_markup(tooltip, tmp);
    gtk_tree_view_set_tooltip_row(tree_view, tooltip, path);
  }

  gtk_tree_path_free(path);
  g_free(tmp);

  return show;
}

static void _is_form_used(const dt_mask_id_t formid,
                          dt_masks_form_t *grp,
                          char *text,
                          const size_t text_length,
                          int *nb)
{
  if(!grp)
  {
    for(const GList *forms = darktable.develop->forms;
        forms;
        forms = g_list_next(forms))
    {
      dt_masks_form_t *form = forms->data;
      if(form->type & DT_MASKS_GROUP) _is_form_used(formid, form, text, text_length, nb);
    }
  }
  else if(grp->type & DT_MASKS_GROUP)
  {
    for(const GList *points = grp->points;
        points;
        points = g_list_next(points))
    {
      dt_masks_point_group_t *point = points->data;
      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, point->formid);
      if(form)
      {
        if(point->formid == formid)
        {
          (*nb)++;
          if(*nb > 1) g_strlcat(text, "\n", text_length);
          g_strlcat(text, grp->name, text_length);
        }

        if(form->type & DT_MASKS_GROUP)
          _is_form_used(formid, form, text, text_length, nb);
      }
    }
  }
}

static void _lib_masks_list_recurs(GtkTreeStore *treestore,
                                   GtkTreeIter *toplevel,
                                   dt_masks_form_t *form,
                                   const int grp_id,
                                   dt_iop_module_t *module,
                                   const int gstate,
                                   const float opacity,
                                   dt_lib_masks_t *lm)
{
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) return;
  // we create the text entry
  char str[256] = "";
  g_strlcat(str, form->name, sizeof(str));
  // we get the right pixbufs
  GdkPixbuf *icop = NULL;
  GdkPixbuf *icinv = NULL;
  GdkPixbuf *icuse = NULL;

  const gboolean show = gstate & DT_MASKS_STATE_SHOW;

  if(gstate & DT_MASKS_STATE_UNION)
    icop = lm->ic_union;
  else if(gstate & DT_MASKS_STATE_INTERSECTION)
    icop = lm->ic_intersection;
  else if(gstate & DT_MASKS_STATE_DIFFERENCE)
    icop = lm->ic_difference;
  else if(gstate & DT_MASKS_STATE_SUM)
    icop = lm->ic_sum;
  else if(gstate & DT_MASKS_STATE_EXCLUSION)
    icop = lm->ic_exclusion;

  if(gstate & DT_MASKS_STATE_INVERSE)
    icinv = lm->ic_inverse;

  char str2[1000] = "";
  int nbuse = 0;
  if(grp_id == 0)
  {
    _is_form_used(form->formid, NULL, str2, sizeof(str2), &nbuse);
    if(nbuse > 0) icuse = lm->ic_used;
  }

  if(!(form->type & DT_MASKS_GROUP))
  {
    // we just add it to the tree
    GtkTreeIter child;

    if(toplevel)
    {
      // we are within a group
      gtk_tree_store_prepend(treestore, &child, toplevel);
    }
    else
    {
      // skip all groups first
      GtkTreeModel *model = GTK_TREE_MODEL(treestore);
      int pos = 0;
      GtkTreeIter iter;

      if(gtk_tree_model_get_iter_first(model, &iter))
      {
        do
        {
          if(gtk_tree_model_iter_has_child(model, &iter))
            ++pos;
        } while(gtk_tree_model_iter_next(model, &iter));
      }

      // insert the child immediately after the last group
      gtk_tree_store_insert(treestore, &child, NULL, pos);
    }

    gtk_tree_store_set(treestore, &child,
                       TREE_TEXT, str,
                       TREE_MODULE, module,
                       TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid,
                       TREE_EDITABLE, (grp_id == 0),
                       TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL) && show,
                       TREE_IC_INVERSE, icinv,
                       TREE_IC_INVERSE_VISIBLE, (icinv != NULL),
                       TREE_IC_USED, icuse,
                       TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2,
                       -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child);
  }
  else
  {
    // we first check if it's a "module" group or not
    if(grp_id == 0 && !module)
    {
      for(const GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
      {
        dt_iop_module_t *iop = iops->data;
        if((iop->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
           && (!(iop->flags() & IOP_FLAGS_NO_MASKS)
               || dt_iop_module_is(iop, "magiceraser"))
           && iop->blend_params->mask_id == form->formid)
        {
          module = iop;
          break;
        }
      }
    }

    // we add the group node to the tree
    GtkTreeIter child;
    gtk_tree_store_prepend(treestore, &child, toplevel);
    gtk_tree_store_set(treestore, &child,
                       TREE_TEXT, str,
                       TREE_MODULE, module,
                       TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid,
                       TREE_EDITABLE, (grp_id == 0),
                       TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL) && show,
                       TREE_IC_INVERSE, icinv,
                       TREE_IC_INVERSE_VISIBLE, (icinv != NULL),
                       TREE_IC_USED, icuse,
                       TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2,
                       -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child);

    // we add all nodes to the tree
    for(const GList *forms = form->points; forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *grpt = forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
      if(f)
        _lib_masks_list_recurs(treestore, &child, f,
                               form->formid, module, grpt->state, grpt->opacity, lm);
    }
  }
}

gboolean _find_mask_iter_by_values(GtkTreeModel *model,
                                   GtkTreeIter *iter,
                                   const dt_iop_module_t *module,
                                   const dt_mask_id_t formid,
                                   const int level)
{
  do
  {
    dt_mask_id_t fid = INVALID_MASKID;
    dt_iop_module_t *mod;
    _lib_masks_get_values(model, iter, &mod, NULL, &fid);
    gboolean found = (fid == formid)
                     && _tree_row_matches_module(mod, module, level);
    if(found) return found;

    GtkTreeIter child, parent = *iter;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _find_mask_iter_by_values(model, &child, module, formid, level + 1);
      if(found)
      {
        *iter = child;
        return found;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));

  return FALSE;
}

GList *_lib_masks_get_selected(dt_lib_module_t *self)
{
  GList *res = NULL;
  dt_lib_masks_t *lm = self->data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  GList *items = gtk_tree_selection_get_selected_rows(selection, &model);

  for(GList *items_iter = items;
      items_iter;
      items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      dt_mask_id_t fid = INVALID_MASKID;
      dt_mask_id_t gid = INVALID_MASKID;
      dt_iop_module_t *mod;
      _lib_masks_get_values(model, &iter, &mod, &gid, &fid);
      res = g_list_prepend(res, GINT_TO_POINTER(fid));
      res = g_list_prepend(res, GINT_TO_POINTER(gid));
      res = g_list_prepend(res, (void *)(mod));
    }
  }

  g_list_foreach(items, (GFunc)gtk_tree_path_free, NULL);
  g_list_free(items);

  return res;
}

// A hash of everything that determines the tree's *structure* (which rows exist
// and how they nest): the image, module instances, all-shapes view, active
// module, and every form's id/type plus each group member's id and parent.
// Module instances must be included because the rows keep owner pointers;
// reusing a tree after a photo switch would leave those pointers dangling.
// Deliberately excludes
// per-shape parameters - opacity, state, size, feather, rotation - so editing
// them (e.g. dragging a slider) leaves the hash unchanged and gui_update can
// refresh the rows in place instead of recreating the store, which would reset
// the panel scroll.
static guint _forms_structure_hash(const gboolean all_shapes)
{
  dt_develop_t *dev = darktable.develop;
  guint h = 2166136261u; // FNV-1a
#define _MIX(v)                                                                                    \
  do                                                                                               \
  {                                                                                                \
    h = (h ^ (guint)(v)) * 16777619u;                                                              \
  } while(0)
  _MIX(dev->image_storage.id);
  _MIX(all_shapes);
  const size_t mod = (size_t)dev->gui_module;
  _MIX(mod);
  _MIX(mod >> 32);
  for(const GList *l = dev->iop; l; l = g_list_next(l))
  {
    const size_t owner = (size_t)l->data;
    _MIX(owner);
    _MIX(owner >> 32);
  }
  for(const GList *l = dev->forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    _MIX(f->formid);
    _MIX(f->type);
    if(f->type & DT_MASKS_GROUP)
      for(const GList *p = f->points; p; p = g_list_next(p))
      {
        const dt_masks_point_group_t *pt = p->data;
        _MIX(pt->formid);
        _MIX(pt->parentid);
      }
  }
#undef _MIX
  return h;
}

void gui_update(dt_lib_module_t *self)
{
  /* first destroy all buttons in list */
  dt_lib_masks_t *lm = self->data;
  if(!lm) return;

  DT_TRY_GUI_UPDATE();

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->show_overlay),
                               darktable.develop->mask_overlay_show);

  // if the tree structure is unchanged (e.g. this update was triggered by a slider
  // edit, which only alters shape parameters), do not recreate the store - that
  // resets the panel scroll and makes the view jump under the cursor. Just refresh
  // the existing rows in place (text/icons), leaving scroll and selection untouched.
  const guint newhash = _forms_structure_hash(_all_shapes_shown(lm));
  if(lm->treeview && lm->tree_hash_valid && newhash == lm->tree_hash &&
     !dt_is_valid_maskid(lm->pending_selectid))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    if(model)
      gtk_tree_model_foreach(model, _update_foreach, lm);
    _overlay_set_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview)));
    DT_LEAVE_GUI_UPDATE();
    return;
  }

  // if a treeview is already present, let's get the currently selected items
  // as we are going to recreate the tree.
  GList *selectids = NULL;

  if(lm->treeview)
  {
    selectids = _lib_masks_get_selected(self);
  }

  _lib_masks_inactivate_icons(self);

  GtkTreeStore *treestore;
  // we store : text ; *module ; groupid ; formid
  treestore = gtk_tree_store_new(TREE_COUNT,
                                 G_TYPE_STRING, G_TYPE_POINTER,
                                 G_TYPE_INT, G_TYPE_INT,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                 G_TYPE_BOOLEAN, G_TYPE_STRING);

  const gboolean all_shapes = _all_shapes_shown(lm);

  // we first add all groups, including localmask-owned ones: selection_change
  // and local adjustments look those children up in this tree
  for(const GList *forms = darktable.develop->forms;
      forms;
      forms = g_list_next(forms))
  {
    dt_masks_form_t *form = forms->data;
    if(form->type & DT_MASKS_GROUP)
      _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm);
  }

  // grouped leaves already appear under their group; the extra top-level copy
  // is inventory for reuse and only shown with all shapes
  for(const GList *forms = darktable.develop->forms;
      forms;
      forms = g_list_next(forms))
  {
    dt_masks_form_t *form = forms->data;
    if(form->type & DT_MASKS_GROUP) continue;
    if(!all_shapes && _form_is_group_member(form->formid)) continue;
    _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm);
  }

  gtk_tree_view_set_model(GTK_TREE_VIEW(lm->treeview), GTK_TREE_MODEL(treestore));

  // select the images as selected in the previous tree
  if(selectids)
  {
    GList *ids = selectids;
    while(ids)
    {
      GtkTreeModel *model = GTK_TREE_MODEL(treestore);
      dt_iop_module_t *mod = ids->data;
      ids = g_list_next(ids);
      // const int gid = GPOINTER_TO_INT(ids->data); // not needed, skip it
      ids = g_list_next(ids);
      const int fid = GPOINTER_TO_INT(ids->data);
      ids = g_list_next(ids);

      GtkTreeIter iter;
      // get formid in group for the given module
      const gboolean found = gtk_tree_model_get_iter_first(model, &iter)
                             && _find_mask_iter_by_values(model, &iter, mod, fid, 1);

      if(found)
      {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview),
                                     path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_path_free(path);
        GtkTreeSelection *selection =
          gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
        gtk_tree_selection_select_iter(selection, &iter);
      }
    }
    g_list_free(selectids);
  }

  // apply a selection that was requested before this row existed (e.g. a shape
  // that was just created): now that the tree is rebuilt, select its row so the
  // new shape shows up as selected in the mask manager.
  if(dt_is_valid_maskid(lm->pending_selectid))
  {
    GtkTreeModel *model = GTK_TREE_MODEL(treestore);
    GtkTreeIter iter;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      gtk_tree_view_expand_all(GTK_TREE_VIEW(lm->treeview));
      if(_lib_masks_selection_change_r(
           model, selection, &iter, lm->pending_selmodule, lm->pending_selectid, 1))
      {
        // make the just-created shape the active one so the properties reflect
        // it, rather than the previously selected shape: mask_form_selected_id
        // is otherwise only set once a shape is clicked on the canvas.
        darktable.develop->mask_form_selected_id = lm->pending_selectid;
        _update_all_properties(lm);

        GList *rows = gtk_tree_selection_get_selected_rows(selection, NULL);
        if(rows)
        {
          // a genuinely new shape: reveal its row
          gtk_tree_view_scroll_to_cell(
            GTK_TREE_VIEW(lm->treeview), rows->data, NULL, TRUE, 0.5, 0.5);
          g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        }
      }
      else
        gtk_tree_view_collapse_all(GTK_TREE_VIEW(lm->treeview));
    }
    lm->pending_selectid = NO_MASKID;
    lm->pending_selmodule = NULL;
  }

  g_object_unref(treestore);

  // remember the structure we just built so a later parameter-only update can be
  // served in place (see the early-out at the top of this function).
  lm->tree_hash = newhash;
  lm->tree_hash_valid = TRUE;

  DT_LEAVE_GUI_UPDATE();

  dt_gui_widget_reallocate_now(lm->treeview);
  _overlay_set_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview)));
}

static void _lib_masks_recreate_list(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;
  dt_lib_gui_queue_update(self);

  DT_TRY_GUI_UPDATE();

  _update_all_properties(lm);

  DT_LEAVE_GUI_UPDATE();

}

static gboolean _update_foreach(GtkTreeModel *model,
                                GtkTreePath *path,
                                GtkTreeIter *iter,
                                gpointer data)
{
  if(!iter) return 0;

  // we retrieve the ids
  dt_mask_id_t grid = INVALID_MASKID;
  dt_mask_id_t id = INVALID_MASKID;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  // we retrieve the forms
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form) return 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);

  // and the values
  int state = 0;
  float opacity = 1.0f;

  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
    {
      dt_masks_point_group_t *pt = pts->data;
      if(pt->formid == id)
      {
        state = pt->state;
        opacity = pt->opacity;
        break;
      }
    }
  }

  _set_iter_name(data, form, state, opacity, model, iter);
  return 0;
}

static void _lib_masks_update_list(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = self->data;
  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  gtk_tree_model_foreach(model, _update_foreach, lm);
}

static gboolean _remove_foreach(GtkTreeModel *model,
                                GtkTreePath *path,
                                GtkTreeIter *iter,
                                gpointer data)
{
  if(!iter) return 0;
  GList **rl = (GList **)data;
  const dt_mask_id_t refid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "formid"));
  const dt_mask_id_t refgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "groupid"));

  dt_mask_id_t grid = INVALID_MASKID;
  dt_mask_id_t id = INVALID_MASKID;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  if(grid == refgid && id == refid)
  {
    GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
    *rl = g_list_append(*rl, rowref);
  }
  return 0;
}

static void _lib_masks_remove_item(dt_lib_module_t *self,
                                   const dt_mask_id_t formid,
                                   const dt_mask_id_t parentid)
{
  dt_lib_masks_t *lm = self->data;
  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GList *rl = NULL;
  g_object_set_data(G_OBJECT(model), "formid", GUINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(model), "groupid", GUINT_TO_POINTER(parentid));
  gtk_tree_model_foreach(model, _remove_foreach, &rl);

  for(const GList *rlt = rl; rlt; rlt = g_list_next(rlt))
  {
    GtkTreeRowReference *rowref = (GtkTreeRowReference *)rlt->data;
    GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
    gtk_tree_row_reference_free(rowref);
    if(path)
    {
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
      {
        gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
      }
      gtk_tree_path_free(path);
    }
  }
  g_list_free(rl);
}

static gboolean _lib_masks_selection_change_r(GtkTreeModel *model,
                                              GtkTreeSelection *selection,
                                              GtkTreeIter *iter,
                                              struct dt_iop_module_t *module,
                                              const dt_mask_id_t selectid,
                                              const int level)
{
  gboolean found = FALSE;

  GtkTreeIter i = *iter;
  do
  {
    dt_mask_id_t id = INVALID_MASKID;
    dt_iop_module_t *mod;
    _lib_masks_get_values(model, &i, &mod, NULL, &id);

    if((id == selectid) && _tree_row_matches_module(mod, module, level))
    {
      gtk_tree_selection_select_iter(selection, &i);
      found = TRUE;
      break;
    }

    // check for children if any
    GtkTreeIter child, parent = i;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _lib_masks_selection_change_r(model, selection,
                                            &child, module, selectid, level + 1);
      if(found)
      {
        break;
      }
    }
  } while(gtk_tree_model_iter_next(model, &i));

  return found;
}

static void _lib_masks_selection_change(dt_lib_module_t *self,
                                        struct dt_iop_module_t *module,
                                        const dt_mask_id_t selectid)
{
  dt_lib_masks_t *lm = self->data;
  if(!lm->treeview) return;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  if(!model) return;

  DT_ENTER_GUI_UPDATE();

  // we first unselect all
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  gtk_tree_selection_unselect_all(selection);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

  // we go through all nodes
  gboolean found = FALSE;
  if(valid)
  {
    gtk_tree_view_expand_all(GTK_TREE_VIEW(lm->treeview));
    found = _lib_masks_selection_change_r(model, selection, &iter, module, selectid, 1);
    if(!found) gtk_tree_view_collapse_all(GTK_TREE_VIEW(lm->treeview));
    else
    {
      GList *rows = gtk_tree_selection_get_selected_rows(selection, NULL);
      if(rows)
      {
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview), rows->data,
                                     NULL, TRUE, 0.5, 0.5);
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
      }
    }
  }

  // a shape just created is not yet in the (still to be rebuilt) tree, so the
  // row can't be selected now; remember it and let gui_update apply it once the
  // tree is rebuilt. Otherwise the request is satisfied, so drop any pending one.
  if(!found && dt_is_valid_maskid(selectid))
  {
    lm->pending_selectid = selectid;
    lm->pending_selmodule = module;
  }
  else
  {
    lm->pending_selectid = NO_MASKID;
    lm->pending_selmodule = NULL;
  }

  DT_LEAVE_GUI_UPDATE();

  _update_all_properties(lm);
  _overlay_set_selected(selection);
  // a just-created shape is not in the tree yet (pending): rebuild the list now
  // instead of waiting for the next lazy panel redraw. Otherwise the new row -
  // and the panel reflow it causes - only appears the first time the user drags
  // a property slider, making the sliders visibly jump. gui_update applies (and
  // clears) the pending selection. dt_lib_gui_update is a no-op unless a rebuild
  // was already queued (dt_dev_masks_list_change, which creation triggers).
  // nested GUI updates skip the rebuild, so preserve its queued dirty flag
  if(dt_is_valid_maskid(lm->pending_selectid) && !DT_IN_GUI_UPDATE())
    dt_lib_gui_update(self);
}

static GdkPixbuf *_get_pixbuf_from_cairo(DTGTKCairoPaintIconFunc paint,
                                         const int width,
                                         const int height)
{
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BUTTON_FG, 1.0);
  paint(cr, 0, 0, width, height, 0, NULL);
  cairo_destroy(cr);
  guchar *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, width, height);
  return gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8, width, height,
                                  cairo_image_surface_get_stride(cst), NULL, NULL);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_masks_t *d = g_malloc0(sizeof(dt_lib_masks_t));
  self->data = (void *)d;

  // initialise all masks pixbuf. This is needed for the "automatic"
  // cell renderer of the treeview
  const int bs2 = DT_PIXEL_APPLY_DPI(13);
  d->ic_inverse = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_inverse, bs2, bs2);
  d->ic_used = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_used, bs2, bs2);
  d->ic_union = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, bs2 * 2, bs2);
  d->ic_intersection =
    _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, bs2 * 2, bs2);
  d->ic_difference =
    _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, bs2 * 2, bs2);
  d->ic_sum =
    _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_sum, bs2 * 2, bs2);
  d->ic_exclusion =
    _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, bs2 * 2, bs2);

  // initialise widgets
  d->bt_gradient = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_masks_gradient, 0, NULL,
    &(dtgtk_button_config_t){
      .tooltip = _("add gradient"),
      .action = DT_ACTION(self),
      .action_section = N_("shapes"),
      .action_label = N_("add gradient"),
      .action_def = &dt_action_def_toggle,
    });
  g_object_set_data(G_OBJECT(d->bt_gradient), DT_ACTION_GESTURE_KEY,
                    dt_gui_connect_click(d->bt_gradient, _bt_add_shape_cb, NULL,
                                         GINT_TO_POINTER(DT_MASKS_GRADIENT)));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_gradient), FALSE);

  d->bt_path = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_masks_path, 0, NULL,
    &(dtgtk_button_config_t){
      .tooltip = _("add path"),
      .action = DT_ACTION(self),
      .action_section = N_("shapes"),
      .action_label = N_("add path"),
      .action_def = &dt_action_def_toggle,
    });
  g_object_set_data(G_OBJECT(d->bt_path), DT_ACTION_GESTURE_KEY,
                    dt_gui_connect_click(d->bt_path, _bt_add_shape_cb, NULL,
                                         GINT_TO_POINTER(DT_MASKS_PATH)));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_path), FALSE);

  d->bt_ellipse = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_masks_ellipse, 0, NULL,
    &(dtgtk_button_config_t){
      .tooltip = _("radial gradient: adjust inside the ellipse by default\n"
                   "invert the mask to adjust outside; shift-drag to make a circle"),
      .action = DT_ACTION(self),
      .action_section = N_("shapes"),
      .action_label = N_("radial gradient"),
      .action_def = &dt_action_def_toggle,
    });
  g_object_set_data(G_OBJECT(d->bt_ellipse), DT_ACTION_GESTURE_KEY,
                    dt_gui_connect_click(d->bt_ellipse, _bt_add_shape_cb, NULL,
                                         GINT_TO_POINTER(DT_MASKS_ELLIPSE)));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_ellipse), FALSE);

  d->bt_circle = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_masks_circle, 0, NULL,
    &(dtgtk_button_config_t){
      .tooltip = _("add circle"),
      .action = DT_ACTION(self),
      .action_section = N_("shapes"),
      .action_label = N_("add circle"),
      .action_def = &dt_action_def_toggle,
    });
  g_object_set_data(G_OBJECT(d->bt_circle), DT_ACTION_GESTURE_KEY,
                    dt_gui_connect_click(d->bt_circle, _bt_add_shape_cb, NULL,
                                         GINT_TO_POINTER(DT_MASKS_CIRCLE)));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_circle), FALSE);
  /* Circle is the round case of the radial gradient, not a second tool. */
  gtk_widget_set_no_show_all(d->bt_circle, TRUE);
  gtk_widget_hide(d->bt_circle);

  d->bt_brush = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_masks_brush, 0, NULL,
    &(dtgtk_button_config_t){
      .tooltip = _("add brush"),
      .action = DT_ACTION(self),
      .action_section = N_("shapes"),
      .action_label = N_("add brush"),
      .action_def = &dt_action_def_toggle,
    });
  g_object_set_data(G_OBJECT(d->bt_brush), DT_ACTION_GESTURE_KEY,
                    dt_gui_connect_click(d->bt_brush, _bt_add_shape_cb, NULL,
                                         GINT_TO_POINTER(DT_MASKS_BRUSH)));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_brush), FALSE);

#ifdef HAVE_AI
  d->bt_object = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_masks_object, 0, NULL,
    &(dtgtk_button_config_t){
      .tooltip = _("add AI object"),
      .action = DT_ACTION(self),
      .action_section = N_("shapes"),
      .action_label = N_("add object"),
      .action_def = &dt_action_def_toggle,
    });
  g_object_set_data(G_OBJECT(d->bt_object), DT_ACTION_GESTURE_KEY,
                    dt_gui_connect_click(d->bt_object, _bt_add_shape_cb, NULL,
                                         GINT_TO_POINTER(DT_MASKS_OBJECT)));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_object), FALSE);
#endif

  d->treeview = gtk_tree_view_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(col, "shapes");
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview), col);

  GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_OP, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_OP_VISIBLE);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_INVERSE, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_INVERSE_VISIBLE);
  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "markup", TREE_TEXT);
  gtk_tree_view_column_add_attribute(col, renderer, "editable", TREE_EDITABLE);
  g_signal_connect(renderer, "edited", G_CALLBACK(_tree_cell_edited), self);
  dt_gui_commit_on_focus_loss(renderer, NULL);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_end(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_USED, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_USED_VISIBLE);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_selection_set_select_function(selection, _tree_restrict_select, d, NULL);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->treeview), FALSE);
  gtk_widget_set_has_tooltip(d->treeview, TRUE);
  g_signal_connect(d->treeview, "query-tooltip", G_CALLBACK(_tree_query_tooltip), NULL);
  g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), d);
  dt_gui_connect_click_all(d->treeview, _tree_button_pressed_cb, NULL, self);
  GtkGesture *copy_drag = dt_gui_connect_drag(d->treeview,
                                              _tree_copy_drag_begin, _tree_copy_drag_end,
                                              _tree_copy_drag_update, self);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(copy_drag), GTK_PHASE_CAPTURE);
  g_signal_connect(copy_drag, "cancel", G_CALLBACK(_tree_copy_drag_cancel), self);
  gtk_widget_add_events(d->treeview, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(d->treeview, "motion-notify-event", G_CALLBACK(_tree_overlay_motion), NULL);
  g_signal_connect(d->treeview, "leave-notify-event", G_CALLBACK(_tree_overlay_leave), NULL);

  d->show_overlay = gtk_check_button_new_with_label(_("show mask overlay"));
  gtk_widget_set_tooltip_text(d->show_overlay,
                              _("show the selected mask's coverage over the photo; hovering a shape previews it temporarily"));
  g_signal_connect(d->show_overlay, "toggled", G_CALLBACK(_show_overlay_toggled), NULL);

  d->all_shapes = gtk_check_button_new_with_label(_("all shapes"));
  gtk_widget_set_tooltip_text(d->all_shapes,
                              _("show standalone copies of shapes that already belong to a group, for reuse across modules"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->all_shapes), FALSE);
  g_signal_connect(d->all_shapes, "toggled", G_CALLBACK(_all_shapes_toggled), self);

  GtkWidget *shape_buttons = dt_gui_hbox
    (dt_gui_expand(dt_ui_label_new(_("created shapes"))),
     d->bt_brush, d->bt_circle, d->bt_ellipse, d->bt_path, d->bt_gradient);
#ifdef HAVE_AI
  dt_gui_box_add(shape_buttons, d->bt_object);
#endif

  /* "%" format subtracts 2 digits. 2 leaves 1% steps; 1 is 10% steps and 0 snaps to 0% or 100%. */
  d->brush_size = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.0002, 1.0, 0, 0.1, 2);
  dt_bauhaus_widget_set_label(d->brush_size, N_("brush"), N_("size"));
  dt_bauhaus_slider_set_format(d->brush_size, "%");
  gtk_widget_set_tooltip_text(d->brush_size,
                              _("width of the brush. scroll over the photo to change it too"));
  g_signal_connect(d->brush_size, "value-changed", G_CALLBACK(_brush_size_changed), d);

  d->brush_feather = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.0, 1.0, 0, 0.34, 2);
  dt_bauhaus_widget_set_label(d->brush_feather, N_("brush"), N_("feather"));
  dt_bauhaus_slider_set_format(d->brush_feather, "%");
  gtk_widget_set_tooltip_text(d->brush_feather,
                              _("softness of the edge. 0 is a hard edge. shift-scroll over the photo to change it too"));
  g_signal_connect(d->brush_feather, "value-changed", G_CALLBACK(_brush_feather_changed), d);

  d->brush_flow = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.01, 1.0, 0, 1.0, 2);
  dt_bauhaus_widget_set_label(d->brush_flow, N_("brush"), N_("flow"));
  dt_bauhaus_slider_set_format(d->brush_flow, "%");
  gtk_widget_set_tooltip_text(d->brush_flow,
                              _("how strong each dab is, as a fraction of density"));
  g_signal_connect(d->brush_flow, "value-changed", G_CALLBACK(_brush_flow_changed), d);

  d->brush_density = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.05, 1.0, 0, 1.0, 2);
  dt_bauhaus_widget_set_label(d->brush_density, N_("brush"), N_("density"));
  dt_bauhaus_slider_set_format(d->brush_density, "%");
  gtk_widget_set_tooltip_text(d->brush_density,
                              _("the strongest the stroke can be"));
  g_signal_connect(d->brush_density, "value-changed", G_CALLBACK(_brush_density_changed), d);

  d->brush_erase = gtk_check_button_new_with_label(_("erase"));
  gtk_widget_set_tooltip_text(d->brush_erase,
                              _("the next stroke removes paint from the selected brush component"));
  g_signal_connect(d->brush_erase, "toggled", G_CALLBACK(_brush_erase_changed), d);

  d->brush_box = dt_gui_vbox(d->brush_size, d->brush_feather, d->brush_flow,
                             d->brush_density, d->brush_erase);
  /* Show the sliders once while no-show-all is still off. Setting the
   * flag first leaves them hidden forever: showing the box later does
   * not show children that were never shown, and the box stays 0px tall. */
  gtk_widget_show_all(d->brush_box);
  gtk_widget_set_no_show_all(d->brush_box, TRUE);
  gtk_widget_hide(d->brush_box);
  g_signal_connect(d->bt_brush, "toggled", G_CALLBACK(_brush_button_toggled), d);

  self->widget = dt_gui_vbox
    (shape_buttons,
     d->brush_box,
     dt_ui_section_label_new(_("advanced")),
     d->all_shapes,
     dt_ui_resize_wrap(d->treeview, 200, "plugins/darkroom/masks/heightview"),
     d->show_overlay);

  dt_gui_new_collapsible_section
    (&d->cs,
     "plugins/darkroom/masks/expand_properties",
     _("properties"),
     GTK_BOX(self->widget),
     DT_ACTION(self));
  d->none_label = dt_ui_label_new(_("no shapes selected"));
  dt_gui_box_add(d->cs.container, d->none_label);
  gtk_widget_show_all(GTK_WIDGET(d->cs.container));
  gtk_widget_set_no_show_all(GTK_WIDGET(d->cs.container), TRUE);

  for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
  {
    GtkWidget *w;
    if(_masks_properties[i].boolean)
    {
      w = gtk_check_button_new_with_label(_(_masks_properties[i].name));
      dt_action_define(DT_ACTION(self), N_("properties"),
                       _masks_properties[i].name, w, &dt_action_def_toggle);
      d->last_value[i] = (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
      g_signal_connect(G_OBJECT(w), "toggled",
                       G_CALLBACK(_property_changed), GINT_TO_POINTER(i));
    }
    else
    {
      w = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                       _masks_properties[i].min,
                                       _masks_properties[i].max,
                                       0, 0.0, 2);
      dt_bauhaus_widget_set_label(w, N_("properties"),
                                  _masks_properties[i].name);
      dt_bauhaus_slider_set_format(w, _masks_properties[i].format);
      dt_bauhaus_slider_set_digits(w, 2);
      if(_masks_properties[i].relative)
        dt_bauhaus_slider_set_log_curve(w);
      d->last_value[i] = dt_bauhaus_slider_get(w);
      g_signal_connect(G_OBJECT(w), "value-changed",
                       G_CALLBACK(_property_changed), GINT_TO_POINTER(i));
    }
    d->property[i] = w;
    dt_gui_box_add(d->cs.container, w);
  }

  d->pressure = dt_gui_preferences_enum(DT_ACTION(self), "pressure_sensitivity");
  dt_bauhaus_widget_set_label(d->pressure, N_("properties"), N_("pressure"));
  d->smoothing = dt_gui_preferences_enum(DT_ACTION(self), "brush_smoothing");
  dt_bauhaus_widget_set_label(d->smoothing, N_("properties"), N_("smoothing"));
  dt_gui_box_add(d->cs.container, d->pressure, d->smoothing);

  // path-only shrink/grow (outset/inset) control: a single signed slider whose
  // quad toggles the unit (px / %). Unlike "size" (a live centroid scaling),
  // this rasterizes and re-vectorizes the outline. The value is a signed offset
  // measured from the shape's baseline; 0 restores it. It commits (debounced) on
  // change. The soft range is ±20 but the hard range is wide so an explicit value
  // can be typed in.
  d->resize_amount = dt_bauhaus_slider_new_action(DT_ACTION(self), -1000, 1000, 1, 0.0, 0);
  dt_bauhaus_widget_set_label(d->resize_amount, N_("properties"), N_("shrink or grow"));
  dt_bauhaus_slider_set_soft_range(d->resize_amount, -20, 20);
  dt_bauhaus_slider_set_format(d->resize_amount, "");
  gtk_widget_set_tooltip_text(d->resize_amount,
                              _("grow (positive) or shrink (negative) the selected path,\n"
                                "relative to its shape when selected; 0 restores the original"));
  g_signal_connect(
    G_OBJECT(d->resize_amount), "value-changed", G_CALLBACK(_resize_amount_changed), d);

  // unit (px / %) toggle in the slider's quad
  dt_bauhaus_widget_set_quad_paint(d->resize_amount, _paint_resize_unit, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(d->resize_amount, TRUE);
  {
    const char *unit = dt_conf_get_string_const("masks/path_resize_unit");
    dt_bauhaus_widget_set_quad_active(d->resize_amount, !g_strcmp0(unit, "% of path size"));
  }
  dt_bauhaus_widget_set_quad_tooltip(
    d->resize_amount, _("shrink/grow unit: image pixels (px) or % of path size — click to toggle"));
  g_signal_connect(G_OBJECT(d->resize_amount), "quad-pressed", G_CALLBACK(_resize_unit_quad), d);
  // initial value suffix reflects the stored unit
  _resize_sync_unit(d);

  d->resize_box = d->resize_amount;
  dt_gui_box_add(d->cs.container, d->resize_amount);
  gtk_widget_show_all(d->resize_amount);
  gtk_widget_set_no_show_all(d->resize_amount, TRUE);

  // "size" scales the shape live; "shrink or grow" insets/outsets its outline.
  // They are the two resize controls, so keep the grow/shrink slider right below
  // "size" instead of at the end of the property list.
  {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(d->cs.container));
    const gint size_pos = g_list_index(kids, d->property[DT_MASKS_PROPERTY_SIZE]);
    if(size_pos >= 0)
      gtk_box_reorder_child(d->cs.container, d->resize_amount, size_pos + 1);
    g_list_free(kids);
  }

  // set proxy functions
  darktable.develop->proxy.masks.module = self;
  darktable.develop->proxy.masks.list_change = _lib_masks_recreate_list;
  darktable.develop->proxy.masks.list_update = _lib_masks_update_list;
  darktable.develop->proxy.masks.list_remove = _lib_masks_remove_item;
  darktable.develop->proxy.masks.selection_change = _lib_masks_selection_change;
  darktable.develop->proxy.masks.local_action = _local_action;
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_masks_t *d = self->data;
  if(d && d->resize_timer)
    g_source_remove(d->resize_timer);
  if(darktable.develop->proxy.masks.module == self)
  {
    darktable.develop->proxy.masks.module = NULL;
    darktable.develop->proxy.masks.list_change = NULL;
    darktable.develop->proxy.masks.list_update = NULL;
    darktable.develop->proxy.masks.list_remove = NULL;
    darktable.develop->proxy.masks.selection_change = NULL;
    darktable.develop->proxy.masks.local_action = NULL;
  }
  darktable.develop->mask_overlay_hover_id = NO_MASKID;
  darktable.develop->mask_overlay_hover_group_id = NO_MASKID;
  darktable.develop->mask_overlay_hover_state = DT_MASKS_STATE_NONE;
  darktable.develop->mask_overlay_hover_owner = NULL;
  darktable.develop->mask_overlay_selected_id = NO_MASKID;
  darktable.develop->mask_overlay_selected_group_id = NO_MASKID;
  darktable.develop->mask_overlay_selected_state = DT_MASKS_STATE_NONE;
  darktable.develop->mask_overlay_selected_owner = NULL;
  darktable.develop->mask_overlay_show = FALSE;
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
