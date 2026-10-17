/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/
#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define POINTCOLOR_MAX_SWATCHES 8
#define POINTCOLOR_SLIDERS 8

DT_MODULE_INTROSPECTION(1, dt_iop_pointcolor_params_t)

typedef struct dt_iop_pointcolor_swatch_t
{
  float L;               // $MIN: 0.0 $MAX: 100.0
  float C;               // $MIN: 0.0 $MAX: 200.0
  float h;               // $MIN: 0.0 $MAX: 1.0
  float hue_range;       // $MIN: 1.0 $MAX: 180.0
  float chroma_range;    // $MIN: 1.0 $MAX: 200.0
  float lightness_range; // $MIN: 1.0 $MAX: 100.0
  float overall_range;   // $MIN: 1.0 $MAX: 200.0
  float hue_shift;       // $MIN: -180.0 $MAX: 180.0
  float chroma_shift;    // $MIN: -100.0 $MAX: 100.0
  float lightness_shift; // $MIN: -100.0 $MAX: 100.0
  float variance;        // $MIN: -100.0 $MAX: 100.0
} dt_iop_pointcolor_swatch_t;

typedef struct dt_iop_pointcolor_params_t
{
  int count; // $MIN: 0 $MAX: 8 $DEFAULT: 0
  dt_iop_pointcolor_swatch_t swatches[POINTCOLOR_MAX_SWATCHES];
} dt_iop_pointcolor_params_t;

typedef struct dt_iop_pointcolor_gui_data_t
{
  GtkWidget *swatch;
  GtkWidget *picker_add;
  GtkWidget *picker_replace;
  GtkWidget *remove;
  GtkWidget *visualize_range;
  GtkWidget *sliders[POINTCOLOR_SLIDERS];
  int selected;
  int adding_index;
  gboolean visualize;
  gboolean updating;
} dt_iop_pointcolor_gui_data_t;

const char *name()
{
  return _("point color");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("refine sampled colors within a mask"),
                                _("creative"),
                                _("non-linear, Lab, display-referred"),
                                _("non-linear, Lab"),
                                _("non-linear, Lab, display-referred"));
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

static inline float _range_weight(const float distance, const float range)
{
  // keep the center stable and feather the last quarter of each range
  const float x = CLAMP((range - distance) / MAX(0.25f * range, 0.001f), 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

static inline float _swatch_weight(const float L, const float C, const float h,
                                  const dt_iop_pointcolor_swatch_t *const s)
{
  const float scale = CLAMP(s->overall_range, 1.0f, 200.0f) * 0.01f;
  const float hdiff = remainderf(h - s->h, 1.0f);
  const float hue_weight = s->C < 2.0f
    ? 1.0f
    : C < 2.0f ? 0.0f
                 : _range_weight(fabsf(hdiff) * 360.0f,
                                 CLAMP(s->hue_range, 1.0f, 180.0f) * scale);
  const float chroma_weight = _range_weight(fabsf(C - s->C),
                                            CLAMP(s->chroma_range, 1.0f, 200.0f) * scale);
  const float lightness_weight = _range_weight(fabsf(L - s->L),
                                               CLAMP(s->lightness_range, 1.0f, 100.0f) * scale);
  return hue_weight * chroma_weight * lightness_weight;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_pointcolor_params_t *const d = piece->data;
  const int count = CLAMP(d->count, 0, POINTCOLOR_MAX_SWATCHES);
  gboolean valid[POINTCOLOR_MAX_SWATCHES] = { FALSE };
  for(int i = 0; i < count; i++)
  {
    const dt_iop_pointcolor_swatch_t *const s = &d->swatches[i];
    valid[i] = isfinite(s->L) && isfinite(s->C) && isfinite(s->h)
               && isfinite(s->hue_range) && isfinite(s->chroma_range)
               && isfinite(s->lightness_range) && isfinite(s->overall_range)
               && isfinite(s->hue_shift) && isfinite(s->chroma_shift)
               && isfinite(s->lightness_shift) && isfinite(s->variance);
  }
  int range_preview_index = -1;
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  if(self->dev->gui_attached && g && dt_pipe_is_full(piece->pipe)
     && dt_iop_has_focus(self))
  {
    dt_iop_gui_enter_critical_section(self);
    if(g->visualize)
      range_preview_index = g->selected;
    dt_iop_gui_leave_critical_section(self);
  }
  if(range_preview_index < 0 || range_preview_index >= count
     || !valid[range_preview_index])
    range_preview_index = -1;
  const float *const restrict in = (const float *)ivoid;
  float *const restrict out = (float *)ovoid;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  // A saved but neutral swatch must be an exact no-op even for values outside
  // nominal Lab gamut; avoid normalizing or clamping its original pixels.
  if(range_preview_index < 0)
  {
    gboolean has_adjustment = FALSE;
    for(int i = 0; i < count; i++)
    {
      if(!valid[i]) continue;
      const dt_iop_pointcolor_swatch_t *const s = &d->swatches[i];
      has_adjustment |= s->hue_shift != 0.0f || s->chroma_shift != 0.0f
                        || s->lightness_shift != 0.0f || s->variance != 0.0f;
    }
    if(!has_adjustment)
    {
      if(out != in) memcpy(out, in, npixels * 4 * sizeof(float));
      return;
    }
  }

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float L = in[4*k];
    const float a = in[4*k + 1];
    const float b = in[4*k + 2];
    const float C = hypotf(a, b);
    const float h = fmodf(atan2f(b, a) + DT_2PI_F, DT_2PI_F) / DT_2PI_F;
    if(range_preview_index >= 0)
    {
      // This is a transient darkroom-only preview. The saved params and export
      // pipeline always use the adjustment path below.
      out[4*k] = 100.0f * _swatch_weight(L, C, h, &d->swatches[range_preview_index]);
      out[4*k + 1] = 0.0f;
      out[4*k + 2] = 0.0f;
      out[4*k + 3] = in[4*k + 3];
      continue;
    }
    float weight_sum = 0.0f;
    float delta_L = 0.0f, delta_a = 0.0f, delta_b = 0.0f;

    for(int i = 0; i < count; i++)
    {
      if(!valid[i]) continue;
      const dt_iop_pointcolor_swatch_t *const s = &d->swatches[i];

      const float weight = _swatch_weight(L, C, h, s);
      if(weight <= 0.0f) continue;
      if(s->hue_shift == 0.0f && s->chroma_shift == 0.0f
         && s->lightness_shift == 0.0f && s->variance == 0.0f)
        continue;

      // variance pulls color toward the sample at negative values and
      // pushes it away at positive values; Lab chroma stands in for saturation
      const float variance = CLAMP(s->variance, -100.0f, 100.0f) * 0.0025f;
      const float hdiff = remainderf(h - s->h, 1.0f);
      const float new_L = L + 0.5f * CLAMP(s->lightness_shift, -100.0f, 100.0f)
                          + variance * (L - s->L);
      const float new_C = MAX(0.0f, C * (1.0f + 0.01f * CLAMP(s->chroma_shift, -100.0f, 100.0f))
                          + variance * (C - s->C));
      const float new_h = h + CLAMP(s->hue_shift, -180.0f, 180.0f) / 360.0f
                          + variance * hdiff;

      weight_sum += weight;
      delta_L += weight * (new_L - L);
      delta_a += weight * (cosf(DT_2PI_F * new_h) * new_C - a);
      delta_b += weight * (sinf(DT_2PI_F * new_h) * new_C - b);
    }

    const float norm = 1.0f / MAX(1.0f, weight_sum);
    out[4*k] = L + delta_L * norm;
    out[4*k + 1] = a + delta_a * norm;
    out[4*k + 2] = b + delta_b * norm;
    out[4*k + 3] = in[4*k + 3];
  }
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
  dt_iop_pointcolor_params_t *const p = self->default_params;
  for(int i = 0; i < POINTCOLOR_MAX_SWATCHES; i++)
  {
    p->swatches[i].hue_range = 45.0f;
    p->swatches[i].chroma_range = 35.0f;
    p->swatches[i].lightness_range = 30.0f;
    p->swatches[i].overall_range = 100.0f;
  }
  memcpy(self->params, p, sizeof(*p));
}

static void _update_gui(dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  const dt_iop_pointcolor_params_t *const p = self->params;
  const int count = CLAMP(p->count, 0, POINTCOLOR_MAX_SWATCHES);
  dt_iop_gui_enter_critical_section(self);
  g->selected = count ? CLAMP(g->selected, 0, count - 1) : -1;
  dt_iop_gui_leave_critical_section(self);
  g->updating = TRUE;
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_combobox_clear(g->swatch);
  for(int i = 0; i < count; i++)
  {
    char label[64];
    snprintf(label, sizeof(label), _("color %d (hue %.0f°)"),
             i + 1, 360.0f * p->swatches[i].h);
    dt_bauhaus_combobox_add(g->swatch, label);
  }
  if(g->selected >= 0)
  {
    const dt_iop_pointcolor_swatch_t *const s = &p->swatches[g->selected];
    const float values[POINTCOLOR_SLIDERS] = { s->hue_shift, s->chroma_shift,
      s->lightness_shift, s->variance, s->overall_range, s->hue_range,
      s->chroma_range, s->lightness_range };
    dt_bauhaus_combobox_set(g->swatch, g->selected);
    for(int i = 0; i < POINTCOLOR_SLIDERS; i++)
      dt_bauhaus_slider_set(g->sliders[i], values[i]);
  }
  gtk_widget_set_sensitive(g->picker_add, count < POINTCOLOR_MAX_SWATCHES);
  gtk_widget_set_sensitive(g->picker_replace, count > 0);
  gtk_widget_set_sensitive(g->remove, count > 0);
  gtk_widget_set_sensitive(g->swatch, count > 0);
  for(int i = 0; i < POINTCOLOR_SLIDERS; i++)
    gtk_widget_set_sensitive(g->sliders[i], count > 0);
  gtk_widget_set_sensitive(g->visualize_range, count > 0);
  DT_LEAVE_GUI_UPDATE();
  g->updating = FALSE;
}

static void _swatch_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  if(g->updating) return;
  dt_iop_gui_enter_critical_section(self);
  g->selected = dt_bauhaus_combobox_get(combo);
  dt_iop_gui_leave_critical_section(self);
  _update_gui(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_iop_refresh_center(self);
}

static void _slider_changed(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  dt_iop_pointcolor_params_t *const p = self->params;
  if(g->updating || g->selected < 0 || g->selected >= p->count) return;
  dt_iop_pointcolor_swatch_t *const s = &p->swatches[g->selected];
  float *fields[POINTCOLOR_SLIDERS] = { &s->hue_shift, &s->chroma_shift,
    &s->lightness_shift, &s->variance, &s->overall_range, &s->hue_range,
    &s->chroma_range, &s->lightness_range };
  for(int i = 0; i < POINTCOLOR_SLIDERS; i++)
  {
    if(slider != g->sliders[i]) continue;
    const float value = dt_bauhaus_slider_get(slider);
    if(fabsf(*fields[i] - value) > 0.0001f)
    {
      *fields[i] = value;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    break;
  }
}

static void _remove_clicked(GtkWidget *button, dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  dt_iop_pointcolor_params_t *const p = self->params;
  if(g->selected < 0 || g->selected >= p->count) return;
  for(int i = g->selected; i < p->count - 1; i++)
    p->swatches[i] = p->swatches[i + 1];
  memset(&p->swatches[--p->count], 0, sizeof(p->swatches[0]));
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  _update_gui(self);
  dt_iop_refresh_center(self);
}

static void _visualize_range_toggled(GtkToggleButton *button, dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  dt_iop_gui_enter_critical_section(self);
  g->visualize = gtk_toggle_button_get_active(button);
  dt_iop_gui_leave_critical_section(self);
  dt_iop_refresh_center(self);
}

static void _add_picker_toggled(GtkToggleButton *button, dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  if(!gtk_toggle_button_get_active(button)) g->adding_index = -1;
}

void color_picker_apply(dt_iop_module_t *self,
                        GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  dt_iop_pointcolor_params_t *const p = self->params;
  if(!g || !isfinite(self->picked_color[0]) || !isfinite(self->picked_color[1])
     || !isfinite(self->picked_color[2])) return;

  int index = g->selected;
  if(picker == g->picker_add)
  {
    if(g->adding_index < 0 || g->adding_index >= p->count)
    {
      if(p->count >= POINTCOLOR_MAX_SWATCHES) return;
      g->adding_index = p->count++;
      memset(&p->swatches[g->adding_index], 0, sizeof(p->swatches[0]));
      p->swatches[g->adding_index].hue_range = 45.0f;
      p->swatches[g->adding_index].chroma_range = 35.0f;
      p->swatches[g->adding_index].lightness_range = 30.0f;
      p->swatches[g->adding_index].overall_range = 100.0f;
    }
    index = g->adding_index;
  }
  else if(picker != g->picker_replace || index < 0 || index >= p->count)
    return;

  dt_iop_pointcolor_swatch_t *const s = &p->swatches[index];
  s->L = CLAMP(self->picked_color[0], 0.0f, 100.0f);
  s->C = CLAMP(self->picked_color[1], 0.0f, 200.0f);
  s->h = self->picked_color[2] - floorf(self->picked_color[2]);
  dt_iop_gui_enter_critical_section(self);
  g->selected = index;
  dt_iop_gui_leave_critical_section(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  _update_gui(self);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
  if(g) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->visualize_range), FALSE);
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  if(!in)
  {
    dt_iop_pointcolor_gui_data_t *const g = self->gui_data;
    if(g) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->visualize_range), FALSE);
    dt_iop_color_picker_reset(self, FALSE);
  }
}

void gui_update(dt_iop_module_t *self)
{
  _update_gui(self);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_pointcolor_gui_data_t *const g = IOP_GUI_ALLOC(pointcolor);
  g->selected = -1;
  g->adding_index = -1;
  g->swatch = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->swatch, NULL, N_("saved color"));
  g->picker_add = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT_AREA,
                                                NULL, IOP_CS_LCH);
  gtk_widget_set_tooltip_text(g->picker_add,
                              _("pick a new color from the image (up to eight)"));
  g->picker_replace = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT_AREA,
                                                    NULL, IOP_CS_LCH);
  gtk_widget_set_tooltip_text(g->picker_replace,
                              _("replace the selected saved color from the image"));
  g->remove = gtk_button_new_with_label(_("remove selected color"));
  g->visualize_range = gtk_check_button_new_with_label(_("visualize range"));
  gtk_widget_set_tooltip_text(g->visualize_range,
                              _("preview this color's selected range in black and white inside the mask; the preview is never exported"));
  const char *labels[POINTCOLOR_SLIDERS] = {
    N_("hue shift"), N_("saturation shift (Lab chroma)"), N_("luminance shift"),
    N_("variance"), N_("range"), N_("hue range"),
    N_("saturation range (Lab chroma)"), N_("luminance range")
  };
  const float min[POINTCOLOR_SLIDERS] = {-180.0f, -100.0f, -100.0f, -100.0f,
                                          1.0f, 1.0f, 1.0f, 1.0f};
  const float max[POINTCOLOR_SLIDERS] = { 180.0f,  100.0f,  100.0f,  100.0f,
                                        200.0f, 180.0f, 200.0f, 100.0f};
  const float defaults[POINTCOLOR_SLIDERS] = {0.0f, 0.0f, 0.0f, 0.0f,
                                               100.0f, 45.0f, 35.0f, 30.0f};
  for(int i = 0; i < POINTCOLOR_SLIDERS; i++)
  {
    g->sliders[i] = dt_bauhaus_slider_new_with_range(self, min[i], max[i], 0.0f,
                                                      defaults[i], 1);
    dt_bauhaus_widget_set_label(g->sliders[i], NULL, labels[i]);
    g_signal_connect(G_OBJECT(g->sliders[i]), "value-changed",
                     G_CALLBACK(_slider_changed), self);
  }
  gtk_widget_set_tooltip_text(g->sliders[3],
                              _("negative pulls colors toward the sample; positive pushes them away"));
  gtk_widget_set_tooltip_text(g->sliders[6],
                              _("this fork uses Lab chroma as a saturation approximation"));

  GtkWidget *const picker_row = dt_gui_hbox(g->picker_add, g->picker_replace);
  self->widget = dt_gui_vbox(g->swatch, picker_row, g->remove, g->visualize_range);
  for(int i = 0; i < POINTCOLOR_SLIDERS; i++)
    dt_gui_box_add(self->widget, g->sliders[i]);

  g_signal_connect(G_OBJECT(g->swatch), "value-changed",
                   G_CALLBACK(_swatch_changed), self);
  g_signal_connect(G_OBJECT(g->remove), "clicked",
                   G_CALLBACK(_remove_clicked), self);
  g_signal_connect(G_OBJECT(g->visualize_range), "toggled",
                   G_CALLBACK(_visualize_range_toggled), self);
  g_signal_connect(G_OBJECT(g->picker_add), "toggled",
                   G_CALLBACK(_add_picker_toggled), self);
  _update_gui(self);
}

// clang-format off
// modelines: These editor modelines have been set by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
