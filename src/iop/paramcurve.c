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

/*
    A Lightroom-style parametric tone curve: four region sliders
    (shadows, darks, lights, highlights) over a 0..1 display-referred
    tone axis cut into regions by three split points. Each slider adds a
    bump supported only inside its region, so black and white stay fixed
    and the curve is flat at every split. The adjusted tone is applied as
    a luminance ratio so hues are preserved. See
    ARCHITECTURE-lightroom-workflow.md (D13) for the contract.
*/

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_paramcurve_params_t)

typedef struct dt_iop_paramcurve_params_t
{
  float shadows;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float darks;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "darks"
  float lights;           // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "lights"
  float highlights;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float split_shadows;    // $MIN: 0.05 $MAX: 0.95 $DEFAULT: 0.25 $DESCRIPTION: "split shadows"
  float split_midtones;   // $MIN: 0.05 $MAX: 0.95 $DEFAULT: 0.5 $DESCRIPTION: "split midtones"
  float split_highlights; // $MIN: 0.05 $MAX: 0.95 $DEFAULT: 0.75 $DESCRIPTION: "split highlights"
} dt_iop_paramcurve_params_t;

// BEGIN PARAMCURVE CORE
// pure curve math, shared verbatim with the headless unit check: no
// darktable headers, only math.h

#define DT_PARAMCURVE_PI 3.14159265358979323846f

// minimum distance kept between two split points, and the narrowest a
// region can get: splits are clamped to [0.05, 0.95] and stay strictly
// ordered with this gap
#define DT_PARAMCURVE_MIN_GAP 0.05f

// peak tone offset of a region slider at +-1 (Lightroom's +-100), reached
// at the middle of its region. the bump sin(pi u)^2 over a region of
// width w has slope magnitude at most pi/w, so with the narrowest
// possible region (DT_PARAMCURVE_MIN_GAP) a peak of MIN_GAP/pi keeps the
// total slope of tone + bumps at >= 0 for every combination of the four
// sliders: the curve stays monotonic. Lightroom does not publish its
// peak height; the F3 harness calibrates this constant later (D13), the
// value here is the largest one that guarantees monotonicity
#define DT_PARAMCURVE_PEAK (DT_PARAMCURVE_MIN_GAP / DT_PARAMCURVE_PI)

// resolution of the precomputed curve table
#define DT_PARAMCURVE_LUT_RES 4096

// tone of a display-referred pixel: Rec. 709 luma
#define DT_PARAMCURVE_LUMA_R 0.2126f
#define DT_PARAMCURVE_LUMA_G 0.7152f
#define DT_PARAMCURVE_LUMA_B 0.0722f

typedef struct dt_iop_paramcurve_core_t
{
  float gain[4];  // shadows, darks, lights, highlights, each -1..1
  float split[3]; // strictly ordered split points, gap >= DT_PARAMCURVE_MIN_GAP
} dt_iop_paramcurve_core_t;

static float _paramcurve_clampf(const float v, const float lo, const float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// clamp the split points into [0.05, 0.95] and force them strictly
// ordered with the minimum gap, cascading upward so even crossed inputs
// yield valid regions
static void _paramcurve_clamp_splits(const float in[3], float out[3])
{
  const float lo = DT_PARAMCURVE_MIN_GAP;
  const float hi = 1.0f - DT_PARAMCURVE_MIN_GAP;
  const float gap = DT_PARAMCURVE_MIN_GAP;
  out[0] = _paramcurve_clampf(in[0], lo, hi - 2.0f * gap);
  out[1] = _paramcurve_clampf(in[1], out[0] + gap, hi - gap);
  out[2] = _paramcurve_clampf(in[2], out[1] + gap, hi);
}

// sin(pi u)^2 on [a, b]: zero at both edges, zero slope at both edges,
// peak 1 at the middle
static float _paramcurve_bump(const float t, const float a, const float b)
{
  if(t <= a || t >= b) return 0.0f;
  const float u = (t - a) / (b - a);
  const float s = sinf(DT_PARAMCURVE_PI * u);
  return s * s;
}

// the parametric curve: identity plus one bump per region; black and
// white are fixed because no bump reaches 0 or 1
static float _paramcurve_eval(const dt_iop_paramcurve_core_t *const c, const float t)
{
  const float e[5] = { 0.0f, c->split[0], c->split[1], c->split[2], 1.0f };
  float y = t;
  for(int r = 0; r < 4; r++)
    y += DT_PARAMCURVE_PEAK * c->gain[r] * _paramcurve_bump(t, e[r], e[r + 1]);
  return y;
}

static void _paramcurve_build_lut(const dt_iop_paramcurve_core_t *const c,
                                  float *const lut,
                                  const int res)
{
  for(int i = 0; i <= res; i++)
    lut[i] = _paramcurve_eval(c, (float)i / (float)res);
}

// t must be inside (0, 1)
static float _paramcurve_lut_eval(const float *const lut, const int res, const float t)
{
  const float x = t * (float)res;
  const int i = (int)x;
  const float f = x - (float)i;
  return lut[i] + f * (lut[i + 1] - lut[i]);
}

static void _paramcurve_apply_pixel(const float *const lut,
                                    const int res,
                                    const float in[4],
                                    float out[4])
{
  const float t = DT_PARAMCURVE_LUMA_R * in[0]
                + DT_PARAMCURVE_LUMA_G * in[1]
                + DT_PARAMCURVE_LUMA_B * in[2];
  // tones outside [0, 1] (and NaN) pass through unchanged
  if(!(t > 0.0f && t < 1.0f))
  {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
    out[3] = in[3];
    return;
  }
  // apply the curve as a luminance ratio so hues stay
  const float ratio = _paramcurve_lut_eval(lut, res, t) / t;
  out[0] = in[0] * ratio;
  out[1] = in[1] * ratio;
  out[2] = in[2] * ratio;
  out[3] = in[3];
}
// END PARAMCURVE CORE

typedef struct dt_iop_paramcurve_data_t
{
  dt_iop_paramcurve_core_t core;
  float lut[DT_PARAMCURVE_LUT_RES + 1];
} dt_iop_paramcurve_data_t;

typedef struct dt_iop_paramcurve_gui_data_t
{
  GtkWidget *shadows, *darks, *lights, *highlights;
  GtkWidget *split_shadows, *split_midtones, *split_highlights;
} dt_iop_paramcurve_gui_data_t;


const char *name()
{
  return _("parametric curve");
}

const char *aliases()
{
  return _("lightroom|tone regions|highlights and shadows");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("adjust shadows, darks, lights and highlights in fixed tone regions"),
     _("creative"),
     _("non-linear, RGB, display-referred"),
     _("non-linear, RGB"),
     _("non-linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_paramcurve_data_t *const d = piece->data;
  const float *const restrict in = (const float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
    _paramcurve_apply_pixel(d->lut, DT_PARAMCURVE_LUT_RES, in + 4 * k, out + 4 * k);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_paramcurve_params_t *const p = (dt_iop_paramcurve_params_t *)p1;
  dt_iop_paramcurve_data_t *const d = piece->data;

  d->core.gain[0] = _paramcurve_clampf(p->shadows, -1.0f, 1.0f);
  d->core.gain[1] = _paramcurve_clampf(p->darks, -1.0f, 1.0f);
  d->core.gain[2] = _paramcurve_clampf(p->lights, -1.0f, 1.0f);
  d->core.gain[3] = _paramcurve_clampf(p->highlights, -1.0f, 1.0f);

  const float splits[3] = { p->split_shadows, p->split_midtones, p->split_highlights };
  _paramcurve_clamp_splits(splits, d->core.split);

  _paramcurve_build_lut(&d->core, d->lut, DT_PARAMCURVE_LUT_RES);
}

// the data struct carries the LUT, so it is far larger than the params
// struct and needs its own aligned allocation; the default cleanup would
// free() it, which mismatches the aligned allocator
void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_paramcurve_data_t);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_paramcurve_gui_data_t *g = IOP_GUI_ALLOC(paramcurve);

  self->widget = dt_gui_vbox();

  // Lightroom panel order: highlights, lights, darks, shadows
  g->highlights = dt_bauhaus_slider_from_params(self, "highlights");
  g->lights = dt_bauhaus_slider_from_params(self, "lights");
  g->darks = dt_bauhaus_slider_from_params(self, "darks");
  g->shadows = dt_bauhaus_slider_from_params(self, "shadows");

  GtkWidget *sliders[4] = { g->highlights, g->lights, g->darks, g->shadows };
  for(int i = 0; i < 4; i++)
  {
    dt_bauhaus_slider_set_digits(sliders[i], 0);
    dt_bauhaus_slider_set_format(sliders[i], "%");
    dt_bauhaus_slider_set_factor(sliders[i], 100.0f);
  }
  gtk_widget_set_tooltip_text(g->highlights,
                              _("adjust the brightest region, above the highlights split"));
  gtk_widget_set_tooltip_text(g->lights,
                              _("adjust the light tones between the midtones and highlights splits"));
  gtk_widget_set_tooltip_text(g->darks,
                              _("adjust the dark tones between the shadows and midtones splits"));
  gtk_widget_set_tooltip_text(g->shadows,
                              _("adjust the darkest region, below the shadows split"));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(_("tone regions")));

  g->split_shadows = dt_bauhaus_slider_from_params(self, "split_shadows");
  g->split_midtones = dt_bauhaus_slider_from_params(self, "split_midtones");
  g->split_highlights = dt_bauhaus_slider_from_params(self, "split_highlights");

  GtkWidget *splits[3] = { g->split_shadows, g->split_midtones, g->split_highlights };
  for(int i = 0; i < 3; i++)
  {
    dt_bauhaus_slider_set_digits(splits[i], 0);
    dt_bauhaus_slider_set_format(splits[i], "%");
    dt_bauhaus_slider_set_factor(splits[i], 100.0f);
    gtk_widget_set_tooltip_text(splits[i],
                                _("move a region boundary on the tone axis\n"
                                  "splits stay strictly ordered with a minimum gap"));
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
