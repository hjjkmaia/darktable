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

#include "develop/lightroom_map.h"

#include "common/introspection.h"
#include "develop/imageop.h"

#include <stdarg.h>

// Lightroom is the standard: scales it does not publish start from these
// structural conversions and are calibrated with the F3 harness
static float _lr_identity(const float lr) { return lr; }
static float _lr_percent(const float lr) { return lr / 100.0f; }
static float _lr_half_percent(const float lr) { return 0.5f * lr / 100.0f; }
// HSL hue degrees: LR -100..100 onto a fraction of colorequal's degrees
static float _lr_hue(const float lr) { return 0.3f * lr; }

// Lightroom's eight HSL colors onto colorequal's bands; Aqua is cyan and
// Purple is lavender there
#define _LR_HSL(lr_color, dt_color)                                                \
  { "HueAdjustment" #lr_color, "colorequal", "hue_" #dt_color, _lr_hue,            \
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },                     \
  { "SaturationAdjustment" #lr_color, "colorequal", "sat_" #dt_color, _lr_percent, \
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },                     \
  { "LuminanceAdjustment" #lr_color, "colorequal", "bright_" #dt_color,            \
    _lr_percent, DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL }

static const dt_lr_mapping_t _map[] =
{
  // D1: LR exposure 0 means "no extra exposure on the working baseline", so
  // it adds to the seeded default (scene-referred +0.7 EV)
  { "Exposure2012", "exposure", "exposure", _lr_identity,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },

  // D2: contrast follows the workflow's tone mapper
  { "Contrast2012", "sigmoid", "middle_grey_contrast", _lr_half_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_SIGMOID },
  { "Contrast2012", "filmicrgb", "contrast", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_FILMIC },
  { "Contrast2012", "agx", "curve_contrast_around_pivot", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_AGX },
  { "Contrast2012", "colorbalancergb", "contrast", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_OTHER },

  // D3: whites/blacks move the tone mapper's targets, as deltas around the
  // defaults; nothing for spektrafilm / display-referred / none
  { "Whites2012", "sigmoid", "display_white_target", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_SIGMOID },
  { "Whites2012", "filmicrgb", "white_point_source", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_FILMIC },
  { "Whites2012", "agx", "range_white_relative_ev", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_AGX },
  { "Blacks2012", "sigmoid", "display_black_target", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_SIGMOID },
  { "Blacks2012", "filmicrgb", "black_point_source", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_FILMIC },
  { "Blacks2012", "agx", "range_black_relative_ev", _lr_percent,
    DT_LR_ADD_TO_DEFAULT | DT_LR_SKIP_IF_ZERO, DT_LR_WF_AGX },

  // D4
  { "Highlights2012", "contrastntexture", "gain_highlights", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "Shadows2012", "contrastntexture", "gain_shadows", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },

  // D5
  { "Texture", "contrastntexture", "gain_medium", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "Clarity2012", "contrastntexture", "gain_coarse", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },

  // D6: hazeremoval's default strength is 0.2, so a zero LR value must not
  // be written at all
  { "Dehaze", "hazeremoval", "strength", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },

  // D7
  { "Vibrance", "colorbalancergb", "vibrance", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "Saturation", "colorbalancergb", "saturation_global", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },

  // D8: colour grade on the 4-ways; hue stays in degrees, sat and lum are
  // percent. one colorbalancergb blob together with D7
  { "ColorGradeShadowHue", "colorbalancergb", "shadows_H", _lr_identity,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeShadowSat", "colorbalancergb", "shadows_C", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeShadowLum", "colorbalancergb", "shadows_Y", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeMidtoneHue", "colorbalancergb", "midtones_H", _lr_identity,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeMidtoneSat", "colorbalancergb", "midtones_C", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeMidtoneLum", "colorbalancergb", "midtones_Y", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeHighlightHue", "colorbalancergb", "highlights_H", _lr_identity,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeHighlightSat", "colorbalancergb", "highlights_C", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeHighlightLum", "colorbalancergb", "highlights_Y", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeGlobalHue", "colorbalancergb", "global_H", _lr_identity,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeGlobalSat", "colorbalancergb", "global_C", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },
  { "ColorGradeGlobalLum", "colorbalancergb", "global_Y", _lr_percent,
    DT_LR_SKIP_IF_ZERO, DT_LR_WF_ALL },

  // D9: HSL on colorequal; sat and brightness are deltas around a default of
  // 1, hue a delta around 0
  _LR_HSL(Red, red),
  _LR_HSL(Orange, orange),
  _LR_HSL(Yellow, yellow),
  _LR_HSL(Green, green),
  _LR_HSL(Aqua, cyan),
  _LR_HSL(Blue, blue),
  _LR_HSL(Purple, lavender),
  _LR_HSL(Magenta, magenta),
};

#undef _LR_HSL

size_t dt_lr_map_lookup(const char *crs_tag, const dt_lr_workflow_t wf,
                        const dt_lr_mapping_t **rows, const size_t max_rows)
{
  size_t n = 0;
  if(!crs_tag) return 0;
  for(size_t i = 0; i < G_N_ELEMENTS(_map); i++)
  {
    if(!g_strcmp0(_map[i].crs_tag, crs_tag) && (_map[i].workflows & wf))
    {
      if(rows && n < max_rows) rows[n] = &_map[i];
      n++;
    }
  }
  return n;
}

dt_lr_workflow_t dt_lr_workflow_from_conf(const char *workflow_conf_value)
{
  if(!g_strcmp0(workflow_conf_value, "scene-referred (sigmoid)"))
    return DT_LR_WF_SIGMOID;
  if(!g_strcmp0(workflow_conf_value, "scene-referred (filmic)"))
    return DT_LR_WF_FILMIC;
  if(!g_strcmp0(workflow_conf_value, "scene-referred (AgX)"))
    return DT_LR_WF_AGX;
  // spektrafilm, display-referred, none, and anything unrecognized
  return DT_LR_WF_OTHER;
}

static void _err(char **err, const char *fmt, ...)
{
  if(!err) return;
  va_list ap;
  va_start(ap, fmt);
  *err = g_strdup_vprintf(fmt, ap);
  va_end(ap);
}

gboolean dt_lr_map_validate(char **err)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_map); i++)
  {
    const dt_lr_mapping_t *m = &_map[i];
    dt_iop_module_so_t *so = m->op ? dt_iop_get_module_so(m->op) : NULL;
    if(!so)
    {
      _err(err, "lightroom_map row %zu ('%s'): unknown module '%s'",
           i, m->crs_tag, m->op ? m->op : "(null)");
      return FALSE;
    }
    if(!so->have_introspection || !so->get_f)
    {
      _err(err, "lightroom_map row %zu ('%s' -> '%s.%s'): module has no introspection",
           i, m->crs_tag, m->op, m->field);
      return FALSE;
    }
    dt_introspection_field_t *f = so->get_f(m->field);
    if(!f)
    {
      _err(err, "lightroom_map row %zu ('%s'): module '%s' has no field '%s'",
           i, m->crs_tag, m->op, m->field);
      return FALSE;
    }
    // every convert yields a float, and ADD_TO_DEFAULT reads and writes the
    // field with get_float/set_float
    if(f->header.type != DT_INTROSPECTION_TYPE_FLOAT)
    {
      _err(err, "lightroom_map row %zu ('%s'): field '%s.%s' is not a float",
           i, m->crs_tag, m->op, m->field);
      return FALSE;
    }
  }
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
