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

#include "develop/iop_params.h"

#include "common/introspection.h"
#include "develop/imageop.h"

#include <stdlib.h>
#include <string.h>

struct dt_iop_params_builder_t
{
  dt_iop_module_so_t *so;
  uint8_t *blob;
  size_t size;
  int version;
};

// is this a flat scalar leaf we can read/write generically?
static gboolean _is_scalar(const dt_introspection_type_t t)
{
  switch(t)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    case DT_INTROSPECTION_TYPE_DOUBLE:
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_INT8:
    case DT_INTROSPECTION_TYPE_UINT8:
    case DT_INTROSPECTION_TYPE_SHORT:
    case DT_INTROSPECTION_TYPE_USHORT:
    case DT_INTROSPECTION_TYPE_BOOL:
    case DT_INTROSPECTION_TYPE_ENUM:
      return TRUE;
    default:
      return FALSE;
  }
}

// write default value of a scalar field into blob at p
static void _write_default(dt_introspection_field_t *f, void *p)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  *(float *)p = f->Float.Default; break;
    case DT_INTROSPECTION_TYPE_DOUBLE: *(double *)p = f->Double.Default; break;
    case DT_INTROSPECTION_TYPE_INT:    *(int *)p = f->Int.Default; break;
    case DT_INTROSPECTION_TYPE_UINT:   *(unsigned int *)p = f->UInt.Default; break;
    case DT_INTROSPECTION_TYPE_INT8:   *(int8_t *)p = f->Int8.Default; break;
    case DT_INTROSPECTION_TYPE_UINT8:  *(uint8_t *)p = f->UInt8.Default; break;
    case DT_INTROSPECTION_TYPE_SHORT:  *(short *)p = f->Short.Default; break;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)p = f->UShort.Default; break;
    case DT_INTROSPECTION_TYPE_BOOL:   *(gboolean *)p = f->Bool.Default; break;
    case DT_INTROSPECTION_TYPE_ENUM:   *(int *)p = f->Enum.Default; break;
    default: break;
  }
}

// the bounds the module's introspection carries. a field whose source has no
// range comment gets its type's full range (introspection.h:77), so this only
// ever rejects what a module actually declares out of bounds
static gboolean _num_in_range(dt_introspection_field_t *f, const double num,
                              double *lo, double *hi)
{
  *lo = 0.0;
  *hi = 0.0;
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  *lo = f->Float.Min;  *hi = f->Float.Max;  break;
    case DT_INTROSPECTION_TYPE_DOUBLE: *lo = f->Double.Min; *hi = f->Double.Max; break;
    case DT_INTROSPECTION_TYPE_INT:    *lo = f->Int.Min;    *hi = f->Int.Max;    break;
    case DT_INTROSPECTION_TYPE_UINT:   *lo = f->UInt.Min;   *hi = f->UInt.Max;   break;
    case DT_INTROSPECTION_TYPE_INT8:   *lo = f->Int8.Min;   *hi = f->Int8.Max;   break;
    case DT_INTROSPECTION_TYPE_UINT8:  *lo = f->UInt8.Min;  *hi = f->UInt8.Max;  break;
    case DT_INTROSPECTION_TYPE_SHORT:  *lo = f->Short.Min;  *hi = f->Short.Max;  break;
    case DT_INTROSPECTION_TYPE_USHORT: *lo = f->UShort.Min; *hi = f->UShort.Max; break;
    default: return TRUE;   // bool and enum carry no range
  }
  return num >= *lo && num <= *hi;
}

// resolve a field name to its introspection entry and the field's address in
// the blob; NULL field means NO_FIELD
static dt_introspection_field_t *_field(const dt_iop_params_builder_t *b,
                                        const char *field, void **p,
                                        dt_iop_params_err_t *err)
{
  dt_introspection_field_t *f = b->so->get_f(field);
  if(!f || !_is_scalar(f->header.type))
  {
    *err = DT_IOP_PARAMS_NO_FIELD;
    return NULL;
  }
  void *addr = b->so->get_p(b->blob, f->header.name);
  if(!addr)
  {
    *err = DT_IOP_PARAMS_NO_FIELD;
    return NULL;
  }
  *p = addr;
  *err = DT_IOP_PARAMS_OK;
  return f;
}

dt_iop_params_builder_t *dt_iop_params_builder_new(const char *op,
                                                   const void *defaults,
                                                   const size_t defaults_size,
                                                   dt_iop_params_err_t *err)
{
  dt_iop_params_err_t e = DT_IOP_PARAMS_OK;
  dt_iop_module_so_t *so = op ? dt_iop_get_module_so(op) : NULL;
  if(!so)
    e = DT_IOP_PARAMS_NO_MODULE;
  else if(!so->have_introspection || !so->get_introspection
          || !so->get_introspection_linear || !so->get_p || !so->get_f)
    e = DT_IOP_PARAMS_NO_INTROSPECTION;
  if(e != DT_IOP_PARAMS_OK)
  {
    if(err) *err = e;
    return NULL;
  }

  dt_introspection_t *intro = so->get_introspection();
  if(defaults && defaults_size != intro->size)
  {
    if(err) *err = DT_IOP_PARAMS_SIZE_MISMATCH;
    return NULL;
  }

  dt_iop_params_builder_t *b = g_malloc0(sizeof(dt_iop_params_builder_t));
  b->so = so;
  b->size = intro->size;
  b->blob = g_malloc0(intro->size);
  b->version = so->version ? so->version() : intro->params_version;

  if(defaults)
  {
    // arrays and curve nodes come across intact, so having one no longer
    // rules out setting a module's scalar fields by name
    memcpy(b->blob, defaults, intro->size);
  }
  else
  {
    dt_introspection_field_t *lin = so->get_introspection_linear();
    // only scalar defaults are reachable here, and a non-scalar left at
    // zero would be worse than refusing
    for(dt_introspection_field_t *f = lin;
        f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
      if(!_is_scalar(f->header.type) && f->header.size != intro->size)
      {
        if(err) *err = DT_IOP_PARAMS_TYPE;
        dt_iop_params_builder_free(b);
        return NULL;
      }

    for(dt_introspection_field_t *f = lin;
        f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
    {
      if(!_is_scalar(f->header.type)) continue;
      void *p = so->get_p(b->blob, f->header.name);
      if(p) _write_default(f, p);
    }
  }

  if(err) *err = DT_IOP_PARAMS_OK;
  return b;
}

dt_iop_params_err_t dt_iop_params_builder_set_float(dt_iop_params_builder_t *b,
                                                    const char *field,
                                                    const float v)
{
  if(!b) return DT_IOP_PARAMS_NO_MODULE;
  dt_iop_params_err_t err = DT_IOP_PARAMS_OK;
  void *p = NULL;
  dt_introspection_field_t *f = _field(b, field, &p, &err);
  if(!f) return err;
  if(f->header.type != DT_INTROSPECTION_TYPE_FLOAT
     && f->header.type != DT_INTROSPECTION_TYPE_DOUBLE)
    return DT_IOP_PARAMS_TYPE;
  double lo = 0.0, hi = 0.0;
  if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
  if(f->header.type == DT_INTROSPECTION_TYPE_FLOAT)
    *(float *)p = v;
  else
    *(double *)p = v;
  return DT_IOP_PARAMS_OK;
}

dt_iop_params_err_t dt_iop_params_builder_set_int(dt_iop_params_builder_t *b,
                                                  const char *field,
                                                  const int v)
{
  if(!b) return DT_IOP_PARAMS_NO_MODULE;
  dt_iop_params_err_t err = DT_IOP_PARAMS_OK;
  void *p = NULL;
  dt_introspection_field_t *f = _field(b, field, &p, &err);
  if(!f) return err;
  double lo = 0.0, hi = 0.0;
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_INT:
      if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
      *(int *)p = v;
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
      *(unsigned int *)p = (unsigned int)v;
      break;
    case DT_INTROSPECTION_TYPE_INT8:
      if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
      *(int8_t *)p = (int8_t)v;
      break;
    case DT_INTROSPECTION_TYPE_UINT8:
      if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
      *(uint8_t *)p = (uint8_t)v;
      break;
    case DT_INTROSPECTION_TYPE_SHORT:
      if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
      *(short *)p = (short)v;
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      if(!_num_in_range(f, v, &lo, &hi)) return DT_IOP_PARAMS_RANGE;
      *(unsigned short *)p = (unsigned short)v;
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean *)p = (v != 0);
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
      *(int *)p = v;
      break;
    default:
      return DT_IOP_PARAMS_TYPE;
  }
  return DT_IOP_PARAMS_OK;
}

dt_iop_params_err_t dt_iop_params_builder_get_float(const dt_iop_params_builder_t *b,
                                                    const char *field, float *v)
{
  if(!b || !v) return DT_IOP_PARAMS_NO_MODULE;
  dt_iop_params_err_t err = DT_IOP_PARAMS_OK;
  void *p = NULL;
  dt_introspection_field_t *f = _field(b, field, &p, &err);
  if(!f) return err;
  if(f->header.type == DT_INTROSPECTION_TYPE_FLOAT)
    *v = *(const float *)p;
  else if(f->header.type == DT_INTROSPECTION_TYPE_DOUBLE)
    *v = (float)*(const double *)p;
  else
    return DT_IOP_PARAMS_TYPE;
  return DT_IOP_PARAMS_OK;
}

const void *dt_iop_params_builder_blob(const dt_iop_params_builder_t *b,
                                       size_t *size, int *version)
{
  if(!b) return NULL;
  if(size) *size = b->size;
  if(version) *version = b->version;
  return b->blob;
}

void dt_iop_params_builder_free(dt_iop_params_builder_t *b)
{
  if(!b) return;
  g_free(b->blob);
  g_free(b);
}

const char *dt_iop_params_err_str(const dt_iop_params_err_t err)
{
  switch(err)
  {
    case DT_IOP_PARAMS_OK:               return "ok";
    case DT_IOP_PARAMS_NO_MODULE:        return "unknown module";
    case DT_IOP_PARAMS_NO_INTROSPECTION: return "module has no introspection";
    case DT_IOP_PARAMS_SIZE_MISMATCH:    return "defaults size != introspection size";
    case DT_IOP_PARAMS_NO_FIELD:         return "unknown field name";
    case DT_IOP_PARAMS_TYPE:             return "field is not a scalar of that type";
    case DT_IOP_PARAMS_RANGE:            return "value outside the field's hard range";
    default:                             return "unknown error";
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
