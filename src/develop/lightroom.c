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

#include "develop/lightroom.h"
#include "common/colorlabels.h"
#include "common/colorspaces.h"
#include "common/curve_tools.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/math.h"
#include "common/metadata.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/iop_params.h"
#include "develop/lightroom_map.h"

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <stdlib.h>
#include <string.h>

// History rows this importer writes carry this multi_name. Re-import
// deletes those rows and writes a fresh set. Preset rows and edits to
// modules this importer did not touch use another name and stay.
// multi_name_hand_edited stays 0, so the darkroom may relabel the
// module on a later edit; the stored row keeps this marker until then.
#define DT_LR_IMPORT_MULTI_NAME "lr-import"

// copy here the iop params struct with the actual version. This is so to
// be as independent as possible of any iop evolutions. Indeed, we create
// the iop params into the database for a specific version. We then ask
// for a reload of the history parameter. If the iop has evolved since then
// the legacy circuitry will be called to convert the parameters.
//
// to add a new iop:
// 1. copy the struct
// 2. add LRDT_<iop_name>_VERSION with corresponding module version
// 3. use this version to pass in dt_add_hist()

#define LRDT_CLIPPING_VERSION 5
typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch, k_h, k_v;
  float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
  int k_type, k_sym;
  int k_apply, crop_auto;
  int ratio_n, ratio_d;
} dt_iop_clipping_params_t;

#define LRDT_FLIP_VERSION 2
typedef struct dt_iop_flip_params_t
{
  dt_image_orientation_t orientation;
} dt_iop_flip_params_t;

#define LRDT_EXPOSURE_VERSION 2
typedef struct dt_iop_exposure_params_t
{
  float black, exposure, gain;
} dt_iop_exposure_params_t;

#define LRDT_GRAIN_VERSION 1
typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE = 0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
} _dt_iop_grain_channel_t;

typedef struct dt_iop_grain_params_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
} dt_iop_grain_params_t;

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,
  DITHER_8BIT = 1,
  DITHER_16BIT = 2
} dt_iop_dither_t;

typedef struct dt_iop_fvector_2d_t
{
  float x;
  float y;
} dt_iop_vector_2d_t;

#define LRDT_VIGNETTE_VERSION 3
typedef struct dt_iop_vignette_params_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering; // if and how to perform dithering
} dt_iop_vignette_params_t;

#define LRDT_SPOTS_VERSION 1
#define MAX_SPOTS 32

typedef struct spot_t
{
  // position of the spot
  float x, y;
  // position to clone from
  float xc, yc;
  float radius;
} spot_t;

typedef struct dt_iop_spots_params_t
{
  int num_spots;
  spot_t spot[MAX_SPOTS];
} dt_iop_spots_params_t;

#define LRDT_TONECURVE_VERSION 3
#define DT_IOP_TONECURVE_MAXNODES 20
typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_max = 3
} tonecurve_channel_t;

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
} dt_iop_tonecurve_params_t;

#define LRDT_SPLITTONING_VERSION 1
typedef struct dt_iop_splittoning_params_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;  // center luminance of gradient
  float compress; // Compress range
} dt_iop_splittoning_params_t;

#define LRDT_BILAT_VERSION 1
typedef struct dt_iop_bilat_params_t
{
  float sigma_r;
  float sigma_s;
  float detail;
} dt_iop_bilat_params_t;


#define LRDT_COLORIN_VERSION 1
#define DT_IOP_COLOR_ICC_LEN_V1 100

typedef struct dt_iop_colorin_params_v1_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN_V1];
  dt_iop_color_intent_t intent;
} dt_iop_colorin_params_v1_t;

#undef DT_IOP_COLOR_ICC_LEN_V1

//
// end of iop structs
//

// the blend params for Lr import, not used in this mode (mode=0), as for iop generate the blend params for
// the
// version specified above.

#define LRDT_BLEND_VERSION 4
#define DEVELOP_BLENDIF_SIZE 16

typedef struct dt_lr_develop_blend_params_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  dt_mask_id_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_lr_develop_blend_params_t;

//
// end of blend_params
//

typedef struct lr2dt
{
  float lr, dt;
} lr2dt_t;

char *dt_get_lightroom_xmp(const dt_imgid_t imgid)
{
  char pathname[DT_MAX_FILENAME_LEN];
  gboolean from_cache = TRUE;

  // Get full pathname
  dt_image_full_path(imgid, pathname, DT_MAX_FILENAME_LEN, &from_cache);

  // Look for extension
  char *pos = strrchr(pathname, '.');

  if(pos == NULL)
    return NULL;

  // If found, replace extension with xmp
  strncpy(pos + 1, "xmp", 4);
  if(g_file_test(pathname, G_FILE_TEST_EXISTS))
    return g_strdup(pathname);

  strncpy(pos + 1, "XMP", 4);
  if(g_file_test(pathname, G_FILE_TEST_EXISTS))
    return g_strdup(pathname);

  return NULL;
}

static float get_interpolate(lr2dt_t lr2dt_table[], const float value)
{
  int k = 0;

  while(lr2dt_table[k + 1].lr < value) k++;

  return lr2dt_table[k].dt
         + ((value - lr2dt_table[k].lr) / (lr2dt_table[k + 1].lr - lr2dt_table[k].lr))
           * (lr2dt_table[k + 1].dt - lr2dt_table[k].dt);
}

static float lr2dt_vignette_gain(const float value)
{
  lr2dt_t lr2dt_vignette_table[] = { { -100, -1 }, { -50, -0.7 }, { 0, 0 }, { 50, 0.5 }, { 100, 1 } };

  return get_interpolate(lr2dt_vignette_table, value);
}

static float lr2dt_vignette_midpoint(const float value)
{
  lr2dt_t lr2dt_vignette_table[] = { { 0, 74 }, { 4, 75 }, { 25, 85 }, { 50, 100 }, { 100, 100 } };

  return get_interpolate(lr2dt_vignette_table, value);
}

static float lr2dt_grain_amount(const float value)
{
  lr2dt_t lr2dt_grain_table[] = { { 0, 0 }, { 25, 20 }, { 50, 40 }, { 100, 80 } };

  return get_interpolate(lr2dt_grain_table, value);
}

static float lr2dt_grain_frequency(const float value)
{
  lr2dt_t lr2dt_grain_table[] = { { 0, 100 }, { 50, 100 }, { 75, 400 }, { 100, 800 } };

  return get_interpolate(lr2dt_grain_table, value) / 53.3;
}

static float lr2dt_splittoning_balance(const float value)
{
  lr2dt_t lr2dt_splittoning_table[] = { { -100, 100 }, { 0, 0 }, { 100, 0 } };

  return get_interpolate(lr2dt_splittoning_table, value);
}

static void dt_add_hist(const dt_imgid_t imgid, const char *operation, const dt_iop_params_t *params, const int params_size, char *imported,
                        const size_t imported_len, const int version, int *import_count)
{
  int32_t num = 0;
  const dt_lr_develop_blend_params_t blend_params = { 0 };

  //  get current num if any
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // add new history info
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history"
                              "  (imgid, num, module, operation, op_params, enabled,"
                              "   blendop_params, blendop_version, multi_priority, multi_name)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7, 0, ?8)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, &blend_params, sizeof(dt_lr_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, LRDT_BLEND_VERSION);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 8, DT_LR_IMPORT_MULTI_NAME, -1, SQLITE_TRANSIENT);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also bump history_end
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = (SELECT IFNULL(MAX(num) + 1, 0)"
                              "                    FROM main.history"
                              "                    WHERE imgid = ?1)"
                              " WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(imported[0]) g_strlcat(imported, ", ", imported_len);
  g_strlcat(imported, dt_iop_get_localized_name(operation), imported_len);
  (*import_count)++;
}

#define MAX_PTS 20

typedef enum lr_curve_kind_t
{
  linear = 0,
  medium_contrast = 1,
  strong_contrast = 2,
  custom = 3
} lr_curve_kind_t;

typedef struct lr_data_t
{
  dt_iop_clipping_params_t pc;
  gboolean has_crop;

  dt_iop_flip_params_t pf;
  gboolean has_flip;

  dt_iop_exposure_params_t pe;
  gboolean has_exposure;

  dt_iop_vignette_params_t pv;
  gboolean has_vignette;

  dt_iop_grain_params_t pg;
  gboolean has_grain;

  dt_iop_spots_params_t ps;
  gboolean has_spots;

  dt_iop_tonecurve_params_t ptc;
  int ptc_value[4];
  float ptc_split[3];
  lr_curve_kind_t curve_kind;
  int curve_pts[MAX_PTS][2];
  int n_pts;

  dt_iop_splittoning_params_t pst;
  gboolean has_splittoning;

  dt_iop_bilat_params_t pbl;
  gboolean has_bilat;

  // crs tags the mapping table owns, name -> gdouble. Darkroom path only.
  GHashTable *mapped;
  // any ColorGrade* tag is present, so the legacy splittoning row is suppressed
  gboolean has_colorgrade;

  gboolean has_tags;

  int rating;
  gboolean has_rating;

  gdouble lat, lon;
  gdouble lat_ref, lon_ref;
  gboolean has_gps;

  int color;
  gboolean has_colorlabel;

  float fratio;                // factor ratio image
  float crop_roundness;        // from lightroom
  int iwidth, iheight;         // image width / height
  dt_exif_image_orientation_t orientation;
} lr_data_t;

// three helper functions for parsing RetouchInfo entries. sscanf doesn't work due to floats.
static gboolean _read_float(const char **startptr, const char *key, float *value)
{
  const char *iter = *startptr;
  while(*iter == ' ') iter++;
  if(!g_str_has_prefix(iter, key))
    return FALSE;
  iter += strlen(key);
  while(*iter == ' ') iter++;
  if(*iter++ != '=')
    return FALSE;
  while(*iter == ' ') iter++;
  *value = g_ascii_strtod(iter, (char **)startptr);
  return iter != *startptr;
}

static gboolean _skip_key_value_pair(const char **startptr, const char *key)
{
  const char *iter = *startptr;
  while(*iter == ' ') iter++;
  if(!g_str_has_prefix(iter, key))
    return FALSE;
  iter += strlen(key);
  while(*iter == ' ') iter++;
  if(*iter++ != '=')
    return FALSE;
  while(*iter == ' ') iter++;
  while((*iter >= 'a' && *iter <= 'z') || (*iter >= 'A' && *iter <= 'Z')) iter++;
  *startptr = iter;
  return TRUE;
}

static gboolean _skip_comma(const char **startptr)
{
  return *(*startptr)++ == ',';
}

/* lrop handle the Lr operation and convert it as a dt iop */
static void _lrop(const dt_develop_t *dev, const xmlDocPtr doc, const dt_imgid_t imgid,
                  const xmlChar *name, const xmlChar *value, const xmlNodePtr node, lr_data_t *data)
{
  if(value)
  {
    // Tags in the mapping table are applied later, one blob per module.
    // Recording them here is what keeps the legacy exposure / clarity
    // writers from also firing: those branches are gone.
    if(dev != NULL && data->mapped)
    {
      const dt_lr_mapping_t *probe[1];
      if(dt_lr_map_lookup((const char *)name, DT_LR_WF_ALL, probe, 1) > 0)
      {
        gdouble *stored = g_new(gdouble, 1);
        *stored = g_ascii_strtod((const char *)value, NULL);
        g_hash_table_replace(data->mapped, g_strdup((const char *)name), stored);
      }
      if(g_str_has_prefix((const char *)name, "ColorGrade"))
        data->has_colorgrade = TRUE;
    }

    if(!xmlStrcmp(name, (const xmlChar *)"CropTop"))
      data->pc.cy = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropRight"))
      data->pc.cw = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropLeft"))
      data->pc.cx = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropBottom"))
      data->pc.ch = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropAngle"))
      data->pc.angle = -g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"ImageWidth"))
      data->iwidth = atoi((char *)value);
    else if(!xmlStrcmp(name, (const xmlChar *)"ImageLength"))
      data->iheight = atoi((char *)value);
    else if(!xmlStrcmp(name, (const xmlChar *)"Orientation"))
    {
      data->orientation = atoi((char *)value);
      if(dev != NULL && ((dev->image_storage.orientation == ORIENTATION_NONE && data->orientation != EXIF_ORIENTATION_NONE)
                        || (dev->image_storage.orientation == ORIENTATION_ROTATE_CW_90_DEG && data->orientation != EXIF_ORIENTATION_ROTATE_CW_90_DEG)
                        || (dev->image_storage.orientation == ORIENTATION_ROTATE_CCW_90_DEG && data->orientation != EXIF_ORIENTATION_ROTATE_CCW_90_DEG)))
        data->has_flip = TRUE;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HasCrop"))
    {
      if(!xmlStrcmp(value, (const xmlChar *)"True")) data->has_crop = TRUE;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteAmount"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_vignette = TRUE;
        data->pv.brightness = lr2dt_vignette_gain((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteMidpoint"))
    {
      const int v = atoi((char *)value);
      data->pv.scale = lr2dt_vignette_midpoint((float)v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteStyle"))
    {
      const int v = atoi((char *)value);
      if(v == 1) // Highlight Priority
        data->pv.saturation = -0.300;
      else // Color Priority & Paint Overlay
        data->pv.saturation = -0.200;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteFeather"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->pv.falloff_scale = (float)v;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteRoundness"))
    {
      const int v = atoi((char *)value);
      data->crop_roundness = (float)v;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GrainAmount"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_grain = TRUE;
        data->pg.strength = lr2dt_grain_amount((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GrainFrequency"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->pg.scale = lr2dt_grain_frequency((float)v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricShadows"))
    {
      data->ptc_value[0] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricDarks"))
    {
      data->ptc_value[1] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricLights"))
    {
      data->ptc_value[2] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricHighlights"))
    {
      data->ptc_value[3] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricShadowSplit"))
    {
      data->ptc_split[0] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricMidtoneSplit"))
    {
      data->ptc_split[1] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricHighlightSplit"))
    {
      data->ptc_split[2] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ToneCurveName2012"))
    {
      if(!xmlStrcmp(value, (const xmlChar *)"Linear"))
        data->curve_kind = linear;
      else if(!xmlStrcmp(value, (const xmlChar *)"Medium Contrast"))
        data->curve_kind = medium_contrast;
      else if(!xmlStrcmp(value, (const xmlChar *)"Strong Contrast"))
        data->curve_kind = strong_contrast;
      else if(!xmlStrcmp(value, (const xmlChar *)"Custom"))
        data->curve_kind = custom;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningShadowHue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.shadow_hue = (float)v / 255.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningShadowSaturation"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.shadow_saturation = (float)v / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningHighlightHue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.highlight_hue = (float)v / 255.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningHighlightSaturation"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.highlight_saturation = (float)v / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningBalance"))
    {
      const float v = g_ascii_strtod((char *)value, NULL);
      data->pst.balance = lr2dt_splittoning_balance(v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Rating"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->rating = v;
        data->has_rating = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLatitudeRef"))
    {
      if(g_str_equal("N", (const char *)value))
      {
        data->lat_ref = 1;
        if(!isnan(data->lat))
        {
          data->lat = (data->lat > 0) ? data->lat : -data->lat;
        }
      }
      else
      {
        data->lat_ref = -1;
        if(!isnan(data->lat))
        {
          data->lat = (data->lat < 0) ? data->lat : -data->lat;
        }
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLongitudeRef"))
    {
      if(g_str_equal("E", (const char *)value))
      {
        data->lon_ref = 1;
        if(!isnan(data->lon))
        {
          data->lon = (data->lon > 0) ? data->lon : -data->lon;
        }
      }
      else
      {
        data->lon_ref = -1;
        if(!isnan(data->lon))
        {
          data->lon = (data->lon < 0) ? data->lon : -data->lon;
        }
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLatitude"))
    {
      const double latitude = dt_util_gps_string_to_number((const char *)value);
      if(!isnan(latitude))
      {
        if(!isnan(data->lat_ref))
        {
          data->lat =  ((latitude > 0) == (data->lat_ref > 0)) ? latitude : -latitude;
        }
        else
        {
          data->lat = latitude;
        }
        data->has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLongitude"))
    {
      const double longitude = dt_util_gps_string_to_number((const char *)value);
      if(!isnan(longitude))
      {
        if(!isnan(data->lon_ref))
        {
          data->lon =  ((longitude > 0) == (data->lon_ref > 0)) ? longitude : -longitude;
        }
        else
        {
          data->lon = longitude;
        }
        data->has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Label"))
    {
      char *v = g_utf8_casefold((char *)value, -1);
      if(!g_strcmp0(v, _("red")))
        data->color = 0;
      else if(!g_strcmp0(v, _("yellow")))
        data->color = 1;
      else if(!g_strcmp0(v, _("green")))
        data->color = 2;
      else if(!g_strcmp0(v, _("blue")))
        data->color = 3;
      else
        // just an else here to catch all other cases as on lightroom one can
        // change the names of labels. So purple and the user's defined labels
        // will be mapped to purple on darktable.
        data->color = 4;

      data->has_colorlabel = TRUE;
      g_free(v);
    }
  }
  if(dev == NULL && (!xmlStrcmp(name, (const xmlChar *)"subject")
                     || !xmlStrcmp(name, (const xmlChar *)"hierarchicalSubject")))
  {
    xmlNodePtr tagNode = node;

    gboolean tag_change = FALSE;
    while(tagNode)
    {
      if(!xmlStrcmp(tagNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, tagNode->xmlChildrenNode, 1);
        guint tagid = 0;
        if(!dt_tag_exists((char *)cvalue, &tagid)) dt_tag_new((char *)cvalue, &tagid);

        if(dt_tag_attach(tagid, imgid, FALSE, FALSE)) tag_change = TRUE;
        data->has_tags = TRUE;
        xmlFree(cvalue);
      }
      tagNode = tagNode->next;
    }
    if(tag_change) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  }
  else if(dev != NULL && !xmlStrcmp(name, (const xmlChar *)"RetouchInfo"))
  {
    xmlNodePtr riNode = node;

    while(riNode)
    {
      if(!xmlStrcmp(riNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, riNode->xmlChildrenNode, 1);
        spot_t *p = &data->ps.spot[data->ps.num_spots];
        float x, y, radius, xc, yc;
        const char *startptr = (const char *)cvalue;
        if(_read_float(&startptr, "centerX", &x) &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "centerY", &y) &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "radius", &radius) &&
           _skip_comma(&startptr) &&
           _skip_key_value_pair(&startptr, "sourceState") &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "sourceX", &xc) &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "sourceY", &yc))
        {
          p->x = x;
          p->y = y;
          p->radius = radius;
          p->xc = xc;
          p->yc = yc;
          data->ps.num_spots++;
          data->has_spots = TRUE;
        }
        xmlFree(cvalue);
      }
      if(data->ps.num_spots == MAX_SPOTS) break;
      riNode = riNode->next;
    }
  }
  else if(dev != NULL && !xmlStrcmp(name, (const xmlChar *)"ToneCurvePV2012"))
  {
    xmlNodePtr tcNode = node;

    while(tcNode)
    {
      if(!xmlStrcmp(tcNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, tcNode->xmlChildrenNode, 1);

        if(sscanf((const char *)cvalue, "%d, %d",
                  &(data->curve_pts[data->n_pts][0]), &(data->curve_pts[data->n_pts][1]))) data->n_pts++;
        xmlFree(cvalue);
      }
      if(data->n_pts == MAX_PTS) break;
      tcNode = tcNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"title"))
  {
    xmlNodePtr ttlNode = node;
    while(ttlNode)
    {
      if(!xmlStrncmp(ttlNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, ttlNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.title", (char *)cvalue);
        xmlFree(cvalue);
      }
      ttlNode = ttlNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"description"))
  {
    xmlNodePtr desNode = node;
    while(desNode)
    {
      if(!xmlStrncmp(desNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, desNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.description", (char *)cvalue);
        xmlFree(cvalue);
      }
      desNode = desNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"creator"))
  {
    xmlNodePtr creNode = node;
    while(creNode)
    {
      if(!xmlStrncmp(creNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, creNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.creator", (char *)cvalue);
        xmlFree(cvalue);
      }
      creNode = creNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"rights"))
  {
    xmlNodePtr rigNode = node;
    while(rigNode)
    {
      if(!xmlStrncmp(rigNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, rigNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.rights", (char *)cvalue);
        xmlFree(cvalue);
      }
      rigNode = rigNode->next;
    }
  }
}

/* _has_list returns true if the node contains a list of value */
static int _has_list(const char *name)
{
  return !strcmp(name, "subject")
    || !strcmp(name, "hierarchicalSubject")
    || !strcmp(name, "RetouchInfo")
    || !strcmp(name, "ToneCurvePV2012")
    || !strcmp(name, "title")
    || !strcmp(name, "description")
    || !strcmp(name, "creator")
    || !strcmp(name, "publisher")
    || !strcmp(name, "rights");
};

/* handle a specific xpath */
static void _handle_xpath(const dt_develop_t *dev, xmlDoc *doc, const dt_imgid_t imgid, xmlXPathContext *ctx, const xmlChar *xpath, lr_data_t *data)
{
  xmlXPathObject *xpathObj = xmlXPathEvalExpression(xpath, ctx);

  if(xpathObj != NULL)
    {
      const xmlNodeSetPtr xnodes = xpathObj->nodesetval;
      const int n = xnodes->nodeNr;

      for(int k=0; k<n; k++)
        {
          const xmlNode *node = xnodes->nodeTab[k];

          if(_has_list((char *)node->name))
            {
              xmlNodePtr listnode = node->xmlChildrenNode;
              if(listnode) listnode = listnode->next;
              if(listnode) listnode = listnode->xmlChildrenNode;
              if(listnode) listnode = listnode->next;
              if(listnode) _lrop(dev, doc, imgid, node->name, NULL, listnode, data);
            }
          else
            {
              const xmlChar *value = xmlNodeListGetString(doc, node->children, 1);
              _lrop(dev, doc, imgid, node->name, value, NULL, data);
            }
        }

      xmlXPathFreeObject(xpathObj);
    }
}

static inline void flip(float *x, float *y)
{
  const float tmp = *x;
  *x = 1.0 - *y;
  *y = 1.0 - tmp;
}

static inline void swap(float *x, float *y)
{
  const float tmp = *x;
  *x = *y;
  *y = tmp;
}

static inline double rotate_x(const double x, const double y, const double rangle)
{
  return x*cos(rangle) + y*sin(rangle);
}

static inline double rotate_y(const double x, const double y, const double rangle)
{
  return -x*sin(rangle) + y*cos(rangle);
}

static inline void rotate_xy(double *cx, double *cy, const double rangle)
{
  const double x = *cx;
  const double y = *cy;
  *cx = rotate_x(x, y, rangle);
  *cy = rotate_y(x, y, rangle);
}

static inline float round5(const double x)
{
  return round(x * 100000.f) / 100000.f;
}

// One history row for a mapped module: current params version, the module's
// current default blend, multi_priority 0. This is the D14 write. It replaces
// the hard-coded LRDT_* version and the legacy v4 blend blob for these rows.
static void _lr_add_hist_current(const dt_imgid_t imgid,
                                 dt_iop_module_t *module,
                                 const void *params,
                                 const size_t params_size,
                                 const int version,
                                 char *imported,
                                 const size_t imported_len,
                                 int *import_count,
                                 const int enabled)
{
  int32_t num = 0;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    num = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history"
                              "  (imgid, num, module, operation, op_params, enabled,"
                              "   blendop_params, blendop_version, multi_priority, multi_name)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, ?8, ?6, ?7, 0, ?9)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, (int)params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, module->default_blendop_params,
                             sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, dt_develop_blend_version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, enabled ? 1 : 0);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 9, DT_LR_IMPORT_MULTI_NAME, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = (SELECT IFNULL(MAX(num) + 1, 0)"
                              "                    FROM main.history"
                              "                    WHERE imgid = ?1)"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(imported[0]) g_strlcat(imported, ", ", imported_len);
  g_strlcat(imported, dt_iop_get_localized_name(module->op), imported_len);
  (*import_count)++;
}

typedef struct _lr_write_t
{
  const dt_lr_mapping_t *row;
  float lr;
} _lr_write_t;

typedef struct _lr_op_acc_t
{
  const char *op;
  GArray *writes;
} _lr_op_acc_t;

static _lr_op_acc_t *_lr_acc(GPtrArray *ops, const char *op)
{
  for(guint i = 0; i < ops->len; i++)
  {
    _lr_op_acc_t *acc = g_ptr_array_index(ops, i);
    if(!g_strcmp0(acc->op, op)) return acc;
  }
  _lr_op_acc_t *acc = g_malloc0(sizeof(_lr_op_acc_t));
  acc->op = op;
  acc->writes = g_array_new(FALSE, FALSE, sizeof(_lr_write_t));
  g_ptr_array_add(ops, acc);
  return acc;
}

static void _lr_acc_free(void *p)
{
  _lr_op_acc_t *acc = p;
  g_array_free(acc->writes, TRUE);
  g_free(acc);
}

static gint _lr_acc_cmp(gconstpointer a, gconstpointer b)
{
  const _lr_op_acc_t *aa = *(const _lr_op_acc_t *const *)a;
  const _lr_op_acc_t *bb = *(const _lr_op_acc_t *const *)b;
  return g_strcmp0(aa->op, bb->op);
}

// Seed one blob per target op from module->default_params, apply every
// non-zero tag that maps onto it, and append one multi_priority 0 row.
// HSL tags land on colorequal; the legacy colorzones writer is gone.
static void _lr_import_mapped(dt_develop_t *dev,
                              const dt_imgid_t imgid,
                              GHashTable *tags,
                              char *imported,
                              const size_t imported_len,
                              int *n_import,
                              gboolean *refresh_needed)
{
  if(!dev || !dev->iop || !tags || g_hash_table_size(tags) == 0) return;

  const dt_lr_workflow_t wf =
    dt_lr_workflow_from_conf(dt_conf_get_string_const("plugins/darkroom/workflow"));

  GPtrArray *ops = g_ptr_array_new_with_free_func(_lr_acc_free);
  GHashTableIter it;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&it, tags);
  while(g_hash_table_iter_next(&it, &key, &value))
  {
    const char *tag = key;
    const float lr = (float)(*(const gdouble *)value);
    const dt_lr_mapping_t *rows[8];
    const size_t n = dt_lr_map_lookup(tag, wf, rows, G_N_ELEMENTS(rows));
    if(n > G_N_ELEMENTS(rows))
      dt_print(DT_DEBUG_ALWAYS,
               "[lightroom] tag '%s' has %zu mappings; only the first %zu are applied",
               tag, n, G_N_ELEMENTS(rows));

    for(size_t i = 0; i < MIN(n, G_N_ELEMENTS(rows)); i++)
    {
      const dt_lr_mapping_t *row = rows[i];
      // a zero Lightroom value contributes nothing, so an op whose tags
      // are all zero never gets a row
      if((row->flags & DT_LR_SKIP_IF_ZERO) && lr == 0.0f)
        continue;

      _lr_write_t w = { row, lr };
      g_array_append_val(_lr_acc(ops, row->op)->writes, w);
    }
  }

  g_ptr_array_sort(ops, _lr_acc_cmp);

  for(guint i = 0; i < ops->len; i++)
  {
    _lr_op_acc_t *acc = g_ptr_array_index(ops, i);
    if(acc->writes->len == 0) continue;

    dt_iop_module_t *module = dt_iop_get_module_by_op_priority(dev->iop, acc->op, 0);
    if(!module || !module->default_params || !module->default_blendop_params)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[lightroom] no priority-0 instance of '%s'; skipped", acc->op);
      continue;
    }

    dt_iop_params_err_t err = DT_IOP_PARAMS_OK;
    dt_iop_params_builder_t *builder = dt_iop_params_builder_new(
      acc->op, module->default_params, (size_t)module->params_size, &err);
    if(!builder)
    {
      dt_print(DT_DEBUG_ALWAYS, "[lightroom] cannot seed '%s': %s",
               acc->op, dt_iop_params_err_str(err));
      continue;
    }

    int applied = 0;
    for(guint w = 0; w < acc->writes->len; w++)
    {
      const _lr_write_t *wr = &g_array_index(acc->writes, _lr_write_t, w);
      float v = wr->row->convert ? wr->row->convert(wr->lr) : wr->lr;
      if(wr->row->flags & DT_LR_ADD_TO_DEFAULT)
      {
        float base = 0.0f;
        err = dt_iop_params_builder_get_float(builder, wr->row->field, &base);
        if(err != DT_IOP_PARAMS_OK)
        {
          dt_print(DT_DEBUG_ALWAYS, "[lightroom] %s.%s: %s",
                   acc->op, wr->row->field, dt_iop_params_err_str(err));
          continue;
        }
        v += base;
      }
      err = dt_iop_params_builder_set_float(builder, wr->row->field, v);
      if(err != DT_IOP_PARAMS_OK)
      {
        dt_print(DT_DEBUG_ALWAYS, "[lightroom] %s.%s = %.6g: %s",
                 acc->op, wr->row->field, v, dt_iop_params_err_str(err));
        continue;
      }
      applied++;
    }

    if(applied > 0)
    {
      size_t size = 0;
      int version = 0;
      const void *blob = dt_iop_params_builder_blob(builder, &size, &version);
      _lr_add_hist_current(imgid, module, blob, size, version,
                           imported, imported_len, n_import, 1);
      *refresh_needed = TRUE;
    }
    dt_iop_params_builder_free(builder);
  }

  g_ptr_array_free(ops, TRUE);
}

static float _lr_clampf(const float v, const float lo, const float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static gboolean _lr_near(const float a, const float b)
{
  const float d = a - b;
  return d < 0.0001f && d > -0.0001f;
}

// same cascade as paramcurve: [0.05, 0.95], strictly ordered, gap >= 0.05
static void _lr_clamp_param_splits(float s[3])
{
  const float gap = 0.05f;
  const float hi = 0.95f;
  s[0] = _lr_clampf(s[0], gap, hi - 2.0f * gap);
  s[1] = _lr_clampf(s[1], s[0] + gap, hi - gap);
  s[2] = _lr_clampf(s[2], s[1] + gap, hi);
}

static gboolean _lr_builder_set(dt_iop_params_builder_t *builder,
                                const char *op,
                                const char *field,
                                const float v)
{
  const dt_iop_params_err_t err = dt_iop_params_builder_set_float(builder, field, v);
  if(err != DT_IOP_PARAMS_OK)
  {
    dt_print(DT_DEBUG_ALWAYS, "[lightroom] %s.%s = %.6g: %s",
             op, field, v, dt_iop_params_err_str(err));
    return FALSE;
  }
  return TRUE;
}

// Parametric* is paramcurve, not a tonecurve node. The point curve
// (ToneCurveName2012 other than Linear) still goes to tonecurve.
// All-zero region sliders and the default splits (25/50/75) write no row.
// paramcurve is off unless a history row enables it; a non-zero region does.
static void _lr_import_paramcurve(dt_develop_t *dev,
                                  const dt_imgid_t imgid,
                                  const lr_data_t *data,
                                  char *imported,
                                  const size_t imported_len,
                                  int *n_import,
                                  gboolean *refresh_needed)
{
  if(!dev || !dev->iop) return;

  const gboolean region = data->ptc_value[0] != 0 || data->ptc_value[1] != 0
                          || data->ptc_value[2] != 0 || data->ptc_value[3] != 0;
  float splits[3] = { data->ptc_split[0], data->ptc_split[1], data->ptc_split[2] };
  _lr_clamp_param_splits(splits);
  if(!region && _lr_near(splits[0], 0.25f) && _lr_near(splits[1], 0.5f)
     && _lr_near(splits[2], 0.75f))
    return;

  dt_iop_module_t *module = dt_iop_get_module_by_op_priority(dev->iop, "paramcurve", 0);
  if(!module || !module->default_params || !module->default_blendop_params)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[lightroom] no priority-0 instance of 'paramcurve'; skipped");
    return;
  }

  dt_iop_params_err_t err = DT_IOP_PARAMS_OK;
  dt_iop_params_builder_t *builder = dt_iop_params_builder_new(
    "paramcurve", module->default_params, (size_t)module->params_size, &err);
  if(!builder)
  {
    dt_print(DT_DEBUG_ALWAYS, "[lightroom] cannot seed 'paramcurve': %s",
             dt_iop_params_err_str(err));
    return;
  }

  // integers are -100..100; the fields are -1..1
  const float regions[4] = {
    _lr_clampf(data->ptc_value[0] / 100.0f, -1.0f, 1.0f),
    _lr_clampf(data->ptc_value[1] / 100.0f, -1.0f, 1.0f),
    _lr_clampf(data->ptc_value[2] / 100.0f, -1.0f, 1.0f),
    _lr_clampf(data->ptc_value[3] / 100.0f, -1.0f, 1.0f),
  };
  static const char *const region_fields[4] = { "shadows", "darks", "lights", "highlights" };
  static const char *const split_fields[3] = {
    "split_shadows", "split_midtones", "split_highlights"
  };

  int applied = 0;
  for(int i = 0; i < 4; i++)
    if(_lr_builder_set(builder, "paramcurve", region_fields[i], regions[i])) applied++;
  for(int i = 0; i < 3; i++)
    if(_lr_builder_set(builder, "paramcurve", split_fields[i], splits[i])) applied++;

  if(applied > 0)
  {
    size_t size = 0;
    int version = 0;
    const void *blob = dt_iop_params_builder_blob(builder, &size, &version);
    _lr_add_hist_current(imgid, module, blob, size, version,
                         imported, imported_len, n_import, region ? 1 : 0);
    *refresh_needed = TRUE;
  }
  dt_iop_params_builder_free(builder);
}

gboolean dt_lightroom_import(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto)
{
  gboolean refresh_needed = FALSE;
  char imported[256] = { 0 };
  int n_import = 0;                // number of iop imported
  gboolean is_lr = FALSE;

  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(imgid);

  if(!pathname)
  {
    if(!iauto) dt_control_log(_("cannot find Lightroom XMP!"));
    return FALSE;
  }

  // Load LR xmp

  xmlDocPtr doc;
  xmlNodePtr entryNode;

  // Parse xml document

  doc = xmlReadFile(pathname, NULL, 0);

  if(doc == NULL)
  {
    g_free(pathname);
    return FALSE ;
  }

  // Enter first node, xmpmeta

  entryNode = xmlDocGetRootElement(doc);

  if(entryNode == NULL)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return FALSE;
  }

  if(xmlStrcmp(entryNode->name, (const xmlChar *)"xmpmeta"))
  {
    if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
    g_free(pathname);
    return FALSE;
  }

  // Check that this is really a Lightroom document

  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);

  if(xpathCtx == NULL)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return FALSE;
  }

  xmlXPathRegisterNs(xpathCtx, BAD_CAST "stEvt", BAD_CAST "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#");

  xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression((const xmlChar *)"//@stEvt:softwareAgent", xpathCtx);

  if(xpathObj == NULL)
  {
    if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
    xmlXPathFreeContext(xpathCtx);
    g_free(pathname);
    xmlFreeDoc(doc);
    return FALSE;
  }

  xmlNodeSetPtr xnodes = xpathObj->nodesetval;

  if(xnodes != NULL && xnodes->nodeNr > 0)
  {
    xmlNodePtr xnode = xnodes->nodeTab[0];
    xmlChar *value = xmlNodeListGetString(doc, xnode->xmlChildrenNode, 1);

    if(!strstr((char *)value, "Lightroom") && !strstr((char *)value, "Camera Raw"))
    {
      xmlXPathFreeContext(xpathCtx);
      xmlXPathFreeObject(xpathObj);
      xmlFreeDoc(doc);
      xmlFree(value);
      if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
      g_free(pathname);
      return FALSE;
    }
    is_lr = TRUE;
    xmlFree(value);
  }
// we could bail out here if we ONLY wanted to load a file known to be from lightroom.
// if we don't know who created it we will just import it however.
//   else
//   {
//     xmlXPathFreeObject(xpathObj);
//     xmlXPathFreeContext(xpathCtx);
//     if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
//     g_free(pathname);
//     return;
//   }

  // let's now parse the needed data

  lr_data_t data;

  memset(&data, 0, sizeof(data));

  data.has_crop = FALSE;
  data.has_flip = FALSE;
  data.has_exposure = FALSE;
  data.has_vignette = FALSE;
  data.has_grain = FALSE;
  data.has_spots = FALSE;
  data.curve_kind = linear;
  data.n_pts = 0;
  // missing Parametric*Split tags mean Lightroom's 25/50/75, not zero
  data.ptc_split[0] = 0.25f;
  data.ptc_split[1] = 0.5f;
  data.ptc_split[2] = 0.75f;
  data.has_splittoning = FALSE;
  data.has_bilat = FALSE;
  data.has_colorgrade = FALSE;
  data.mapped = NULL;
  if(dev != NULL)
    data.mapped = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  data.has_tags = FALSE;
  data.rating = 0;
  data.has_rating = FALSE;
  data.lat = NAN;
  data.lon = NAN;
  data.lat_ref = NAN;
  data.lon_ref = NAN;
  data.has_gps = FALSE;
  data.color = 0;
  data.has_colorlabel = FALSE;
  data.fratio = NAN;                // factor ratio image
  data.crop_roundness = NAN;        // from lightroom
  data.iwidth = 0;
  data.iheight = 0;                 // image width / height
  data.orientation = EXIF_ORIENTATION_NONE;

  // record the name-spaces needed for the parsing
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "crs",
     BAD_CAST "http://ns.adobe.com/camera-raw-settings/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "dc",
     BAD_CAST "http://purl.org/dc/elements/1.1/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "tiff",
     BAD_CAST "http://ns.adobe.com/tiff/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "xmp",
     BAD_CAST "http://ns.adobe.com/xap/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "exif",
     BAD_CAST "http://ns.adobe.com/exif/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "lr",
     BAD_CAST "http://ns.adobe.com/lightroom/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "rdf",
     BAD_CAST "http://www.w3.org/1999/02/22-rdf-syntax-ns#");

  // All prefixes to parse from the XMP document
  static char *names[] = { "crs", "dc", "tiff", "xmp", "exif", "lr", NULL };

  for(int i=0; names[i]!=NULL; i++)
    {
      char expr[50];

      /* Lr 7.0 CC (nodes) */
      snprintf(expr, sizeof(expr), "//%s:*", names[i]);
      _handle_xpath(dev, doc, imgid, xpathCtx, (const xmlChar *)expr, &data);

      /* Lr up to 6.0 (attributes) */
      snprintf(expr, sizeof(expr), "//@%s:*", names[i]);
      _handle_xpath(dev, doc, imgid, xpathCtx, (const xmlChar *)expr, &data);
    }

  xmlXPathFreeObject(xpathObj);
  xmlXPathFreeContext(xpathCtx);
  xmlFreeDoc(doc);

  //  Integrates into the history all the imported iop

  if(dev != NULL && data.mapped)
    _lr_import_mapped(dev, imgid, data.mapped, imported, sizeof(imported),
                      &n_import, &refresh_needed);

  if(dev != NULL && is_lr && dt_image_is_raw(&dev->image_storage))
  {
    // set colorin to cmatrix which is the default from Adobe (so closer to what Lightroom does)
    dt_iop_colorin_params_v1_t pci = (dt_iop_colorin_params_v1_t){ "cmatrix", DT_INTENT_PERCEPTUAL };

    dt_add_hist(imgid, "colorin", (dt_iop_params_t *)&pci, sizeof(dt_iop_colorin_params_v1_t), imported,
                sizeof(imported), LRDT_COLORIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_crop)
  {
    double rangle;
    double cx, cw, cy, ch;
    double new_width, new_height;
    dt_image_orientation_t orientation = dt_image_orientation_to_flip_bits(data.orientation);

    data.pc.k_sym = 0;
    data.pc.k_apply = 0;
    data.pc.crop_auto = 0;  // Cannot use crop-auto=1 (the default at clipping GUI), as it does not allow to cover all cropping cases.
    data.pc.ratio_n = data.pc.ratio_d = -2;
    data.pc.k_h = data.pc.k_v = 0;
    data.pc.k_type = 0;
    data.pc.kxa = data.pc.kxd = 0.2f;
    data.pc.kxc = data.pc.kxb = 0.8f;
    data.pc.kya = data.pc.kyb = 0.2f;
    data.pc.kyc = data.pc.kyd = 0.8f;

    // Convert image in image-centered coordinate system, [-image_size / 2; + image_size / 2]
    cx = (data.pc.cx - 0.5f) * data.iwidth;
    cw = (data.pc.cw - 0.5f) * data.iwidth;
    cy = (data.pc.cy - 0.5f) * data.iheight;
    ch = (data.pc.ch - 0.5f) * data.iheight;

    // Rotate the cropped zone according to rotation angle
    // All rotations done using center of the image
    rangle = deg2rad(data.pc.angle);
    rotate_xy(&cx, &cy, -rangle);
    rotate_xy(&cw, &ch, -rangle);

    // Calculate the new overall image size (black zone included) after rotation
    // rangle is limited to -45°;+45° by LR
    new_width  = rotate_x(+data.iwidth, -data.iheight, -fabs(rangle));
    new_height = rotate_y(+data.iwidth, +data.iheight, -fabs(rangle));

    // apply new size & convert image back in initial coordinate system [0.0 ; +1.0]
    data.pc.cx = round5((cx / new_width)  + 0.5f);
    data.pc.cw = round5((cw / new_width)  + 0.5f);
    data.pc.cy = round5((cy / new_height) + 0.5f);
    data.pc.ch = round5((ch / new_height) + 0.5f);

    // adjust crop data according to the orientation - Must be done after rotation
    if(orientation & ORIENTATION_FLIP_X)
      flip(&data.pc.cx, &data.pc.cw);
    if(orientation & ORIENTATION_FLIP_Y)
      flip(&data.pc.cy, &data.pc.ch);
    if(orientation & ORIENTATION_SWAP_XY)
    {
      swap(&data.pc.cx, &data.pc.cy);
      swap(&data.pc.cw, &data.pc.ch);
    }

    // Invert angle when orientation is flipped
    if(orientation == ORIENTATION_FLIP_HORIZONTALLY
    || orientation == ORIENTATION_FLIP_VERTICALLY
    || orientation == ORIENTATION_TRANSPOSE
    || orientation == ORIENTATION_TRANSVERSE)
      data.pc.angle = -data.pc.angle;

    data.fratio = (data.pc.cw - data.pc.cx) / (data.pc.ch - data.pc.cy);

    dt_add_hist(imgid, "clipping", (dt_iop_params_t *)&data.pc, sizeof(dt_iop_clipping_params_t), imported,
                sizeof(imported), LRDT_CLIPPING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_flip)
  {
    data.pf.orientation = dt_image_orientation_to_flip_bits(data.orientation);

    dt_add_hist(imgid, "flip", (dt_iop_params_t *)&data.pf, sizeof(dt_iop_flip_params_t), imported,
                sizeof(imported), LRDT_FLIP_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  // Exposure2012 and Blacks2012 are mapped (exposure slider, tone-mapper
  // targets). The legacy v2 exposure blob, which also wrote exposure.black,
  // is not emitted.

  if(dev != NULL && data.has_grain)
  {
    data.pg.channel = 0;

    dt_add_hist(imgid, "grain", (dt_iop_params_t *)&data.pg, sizeof(dt_iop_grain_params_t), imported,
                sizeof(imported), LRDT_GRAIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_vignette)
  {
    const float base_ratio = 1.325 / 1.5;

    data.pv.autoratio = FALSE;
    data.pv.dithering = DITHER_8BIT;
    data.pv.center.x = 0.0;
    data.pv.center.y = 0.0;
    data.pv.shape = 1.0;

    // defensive code, should not happen, but just in case future Lr version
    // has not ImageWidth/ImageLength XML tag.
    if(data.iwidth == 0 || data.iheight == 0)
      data.pv.whratio = base_ratio;
    else
      data.pv.whratio = base_ratio * ((float)data.iwidth / (float)data.iheight);

    if(data.has_crop) data.pv.whratio = data.pv.whratio * data.fratio;

    //  Adjust scale and ratio based on the roundness. On Lightroom changing
    //  the roundness change the width and the height of the vignette.

    if(data.crop_roundness > 0)
    {
      float newratio = data.pv.whratio - (data.pv.whratio - 1) * (data.crop_roundness / 100.0);
      float dscale = (1 - (newratio / data.pv.whratio)) / 2.0;

      data.pv.scale -= dscale * 100.0;
      data.pv.whratio = newratio;
    }

    dt_add_hist(imgid, "vignette", (dt_iop_params_t *)&data.pv, sizeof(dt_iop_vignette_params_t), imported,
                sizeof(imported), LRDT_VIGNETTE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_spots)
  {
    // Check for orientation, rotate when in portrait mode
    if(data.orientation > 4)
      for(int k = 0; k < data.ps.num_spots; k++)
      {
        float tmp = data.ps.spot[k].y;
        data.ps.spot[k].y = 1.0 - data.ps.spot[k].x;
        data.ps.spot[k].x = tmp;
        tmp = data.ps.spot[k].yc;
        data.ps.spot[k].yc = 1.0 - data.ps.spot[k].xc;
        data.ps.spot[k].xc = tmp;
      }

    dt_add_hist(imgid, "spots", (dt_iop_params_t *)&data.ps, sizeof(dt_iop_spots_params_t), imported,
                sizeof(imported), LRDT_SPOTS_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  // Parametric* lands on paramcurve. It must not add tonecurve nodes.
  if(dev != NULL)
    _lr_import_paramcurve(dev, imgid, &data, imported, sizeof(imported),
                          &n_import, &refresh_needed);

  // A point curve (ToneCurveName2012 other than Linear, ToneCurvePV2012)
  // still imports on its own module.
  if(dev != NULL && data.curve_kind != linear)
  {
    const int total_pts = (data.curve_kind == custom) ? data.n_pts : 6;
    data.ptc.tonecurve_nodes[ch_L] = total_pts;
    data.ptc.tonecurve_nodes[ch_a] = 7;
    data.ptc.tonecurve_nodes[ch_b] = 7;
    data.ptc.tonecurve_type[ch_L] = CUBIC_SPLINE;
    data.ptc.tonecurve_type[ch_a] = CUBIC_SPLINE;
    data.ptc.tonecurve_type[ch_b] = CUBIC_SPLINE;
    data.ptc.tonecurve_autoscale_ab = 1;
    data.ptc.tonecurve_preset = 0;

    float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

    // linear a, b curves
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_a][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_a][k].y = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_b][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_b][k].y = linear_ab[k];

    for(int k = 0; k < total_pts; k++)
    {
      data.ptc.tonecurve[ch_L][k].x = data.curve_pts[k][0] / 255.0;
      data.ptc.tonecurve[ch_L][k].y = data.curve_pts[k][1] / 255.0;
    }

    dt_add_hist(imgid, "tonecurve", (dt_iop_params_t *)&data.ptc, sizeof(dt_iop_tonecurve_params_t), imported,
                sizeof(imported), LRDT_TONECURVE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  // Color grade owns these files. The legacy splittoning row divides hue
  // by 255, so hue 210 would land near 297 degrees. Keep splittoning only
  // for an old sidecar that has SplitToning* and no ColorGrade*.
  if(dev != NULL && data.has_splittoning && !data.has_colorgrade)
  {
    data.pst.compress = 50.0;

    dt_add_hist(imgid, "splittoning", (dt_iop_params_t *)&data.pst, sizeof(dt_iop_splittoning_params_t), imported,
                sizeof(imported), LRDT_SPLITTONING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  // Clarity2012 is contrastntexture.gain_coarse. No bilat row.

  if(data.has_tags)
  {
    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("tags"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && data.has_rating)
  {
    dt_ratings_apply_on_image(imgid, data.rating, FALSE, FALSE, FALSE);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("rating"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && data.has_gps)
  {
    dt_image_geoloc_t geoloc;
    geoloc.longitude = data.lon;
    geoloc.latitude = data.lat;
    geoloc.elevation = NAN;
    dt_image_set_location(imgid, &geoloc, FALSE, FALSE);
    GList *imgs = NULL;
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("geotagging"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && data.has_colorlabel)
  {
    dt_colorlabels_set_label(imgid, data.color);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("color label"), sizeof(imported));
    n_import++;
  }

  if(data.mapped) g_hash_table_destroy(data.mapped);

  if(dev != NULL && refresh_needed && dev->gui_attached)
  {
    dt_control_log(ngettext("%s has been imported", "%s have been imported", n_import), imported);

    if(!iauto)
    {
      /* signal history changed */
      dt_dev_reload_history_items(dev);
      dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
      /* update xmp file */
      dt_image_synch_xmp(imgid);
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    }
  }
  return TRUE;
}

// Delete importer rows and pack the surviving nums from 0. The append
// path uses COUNT(*) as the next num, which collides if a gap is left
// where a row was removed.
static void _lr_drop_imported_history(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  dt_database_start_transaction(darktable.db);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history"
                              " WHERE imgid = ?1 AND multi_name = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, DT_LR_IMPORT_MULTI_NAME, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.history SET num = num + 1000000 WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid FROM main.history WHERE imgid = ?1 ORDER BY num",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  GArray *rowids = g_array_new(FALSE, FALSE, sizeof(int));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int rowid = sqlite3_column_int(stmt, 0);
    g_array_append_val(rowids, rowid);
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.history SET num = ?1 WHERE rowid = ?2",
                              -1, &stmt, NULL);
  for(guint i = 0; i < rowids->len; i++)
  {
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(stmt);
    DT_DEBUG_SQLITE3_RESET(stmt);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, (int)i);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, g_array_index(rowids, int, i));
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  g_array_free(rowids, TRUE);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = (SELECT IFNULL(MAX(num) + 1, 0)"
                              "                    FROM main.history"
                              "                    WHERE imgid = ?1)"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_database_release_transaction(darktable.db);
}

gboolean dt_lightroom_reimport(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto)
{
  if(!dev)
  {
    if(!iauto)
      dt_control_log(_("re-import needs an open image in the darkroom"));
    return FALSE;
  }

  // Confirm the photo-flavor sidecar is there before deleting anything.
  char *pathname = dt_get_lightroom_xmp(imgid);
  if(!pathname)
  {
    if(!iauto) dt_control_log(_("cannot find Lightroom XMP!"));
    return FALSE;
  }
  g_free(pathname);

  dt_lock_image(imgid);
  _lr_drop_imported_history(imgid);
  const gboolean ok = dt_lightroom_import(imgid, dev, iauto);
  // A GUI reload rewrites the hash. Headless does not reload, so write
  // the current hash here or the next open treats the stack as stale.
  if(ok && !dev->gui_attached)
    dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
  dt_unlock_image(imgid);

  // import reloads only when it appended rows. A sidecar that now
  // imports nothing still has to drop the old rows from the open stack.
  if(ok && dev->gui_attached)
    dt_dev_reload_history_items(dev);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
