/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "iop/magiceraser_result.h"

#include <math.h>
#include <string.h>

static const float source[12] = {
  0.10f, 0.20f, 0.30f, 0.20f, 0.30f, 0.40f,
  0.30f, 0.40f, 0.50f, 0.40f, 0.50f, 0.60f
};
static const float proposed[12] = {
  0.90f, 0.90f, 0.90f, 0.80f, 0.80f, 0.80f,
  0.70f, 0.70f, 0.70f, 0.60f, 0.60f, 0.60f
};
static const float coverage[4] = { 0.0f, 1.0f, 0.25f, 0.0f };

static dt_magiceraser_rgb_result_t _result(const float *values)
{
  return (dt_magiceraser_rgb_result_t){ 5, 7, 2, 2, 3,
    DT_MAGICERASER_RGB_ENCODED_SRGB, 6, 12, values };
}

static void _reset(float *filled)
{
  for(int i = 0; i < 12; i++) filled[i] = -1.0f;
}

static void _reject_unchanged(const dt_magiceraser_rgb_result_t *result,
                              const float *src, const float *mask)
{
  float filled[12];
  _reset(filled);
  g_assert_false(dt_magiceraser_result_apply(5, 7, 2, 2, 10, 10,
                                              src, mask, filled, result));
  for(int i = 0; i < 12; i++) g_assert_cmpfloat(filled[i], ==, -1.0f);
}

static void test_valid_clip_and_unblended_color(void)
{
  float filled[12];
  _reset(filled);
  const dt_magiceraser_rgb_result_t result = _result(proposed);
  g_assert_true(dt_magiceraser_result_apply(5, 7, 2, 2, 10, 10,
                                             source, coverage, filled, &result));
  g_assert_cmpmem(filled, 3 * sizeof(float), source, 3 * sizeof(float));
  g_assert_cmpmem(filled + 3, 6 * sizeof(float), proposed + 3, 6 * sizeof(float));
  g_assert_cmpmem(filled + 9, 3 * sizeof(float), source + 9, 3 * sizeof(float));
}

static void test_exact_origin_shape_and_layout(void)
{
  dt_magiceraser_rgb_result_t result = _result(proposed);
  result.x++;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.width++;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.height--;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.channels = 4;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.encoding = DT_MAGICERASER_RGB_UNKNOWN;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.row_stride_floats++;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.float_count--;
  _reject_unchanged(&result, source, coverage);
  result = _result(proposed);
  result.values = NULL;
  _reject_unchanged(&result, source, coverage);

  float filled[12];
  _reset(filled);
  result = _result(proposed);
  g_assert_false(dt_magiceraser_result_apply(9, 7, 2, 2, 10, 10,
                                              source, coverage, filled, &result));
  for(int i = 0; i < 12; i++) g_assert_cmpfloat(filled[i], ==, -1.0f);
}

static void test_invalid_values_are_transactional(void)
{
  float bad[12];
  memcpy(bad, proposed, sizeof(bad));
  bad[11] = NAN;
  dt_magiceraser_rgb_result_t result = _result(bad);
  _reject_unchanged(&result, source, coverage);
  bad[11] = INFINITY;
  _reject_unchanged(&result, source, coverage);
  bad[11] = -0.01f;
  _reject_unchanged(&result, source, coverage);
  bad[11] = 1.01f;
  _reject_unchanged(&result, source, coverage);

  float invalid_mask[4];
  memcpy(invalid_mask, coverage, sizeof(invalid_mask));
  invalid_mask[3] = NAN;
  result = _result(proposed);
  _reject_unchanged(&result, source, invalid_mask);
  memset(invalid_mask, 0, sizeof(invalid_mask));
  _reject_unchanged(&result, source, invalid_mask);

  float invalid_source[12];
  memcpy(invalid_source, source, sizeof(invalid_source));
  invalid_source[11] = INFINITY;
  _reject_unchanged(&result, invalid_source, coverage);
}

static void test_nearby_fill_small_painted_area(void)
{
  enum { W = 128, H = 128 };
  float *rgb = g_new(float, 3 * W * H);
  float *mask = g_new0(float, W * H);
  float *proposal = g_new(float, 3 * W * H);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const size_t i = (size_t)y * W + x;
      rgb[3 * i] = 0.3f;
      rgb[3 * i + 1] = 0.4f;
      rgb[3 * i + 2] = 0.5f;
      if(x >= 60 && x < 68 && y >= 60 && y < 68)
      {
        rgb[3 * i] = 0.95f;
        mask[i] = 1.0f;
      }
      proposal[3 * i] = proposal[3 * i + 1] = proposal[3 * i + 2] = -1.0f;
    }
  char *error = NULL;
  g_assert_true(dt_magiceraser_nearby_fill(rgb, mask, W, H,
                                            proposal, &error));
  g_assert_null(error);
  for(size_t i = 0; i < W * H; i++)
  {
    if(mask[i] > 0.0f)
    {
      g_assert_cmpfloat(proposal[3 * i], ==, 0.3f);
      g_assert_cmpfloat(proposal[3 * i + 1], ==, 0.4f);
      g_assert_cmpfloat(proposal[3 * i + 2], ==, 0.5f);
    }
    else
      g_assert_cmpmem(proposal + 3 * i, 3 * sizeof(float),
                      rgb + 3 * i, 3 * sizeof(float));
  }
  g_free(rgb);
  g_free(mask);
  g_free(proposal);
}

static void test_nearby_fill_rejects_without_mutation(void)
{
  enum { W = 160, H = 160 };
  float *rgb = g_new0(float, 3 * W * H);
  float *mask = g_new0(float, W * H);
  float *proposal = g_new(float, 3 * W * H);
  for(size_t i = 0; i < 3 * W * H; i++) proposal[i] = -1.0f;
  char *error = NULL;
  g_assert_false(dt_magiceraser_nearby_fill(rgb, mask, W, H,
                                             proposal, &error));
  g_assert_nonnull(error);
  g_clear_pointer(&error, g_free);

  for(int y = 20; y < 130; y++)
    for(int x = 20; x < 130; x++) mask[(size_t)y * W + x] = 1.0f;
  g_assert_false(dt_magiceraser_nearby_fill(rgb, mask, W, H,
                                             proposal, &error));
  g_assert_nonnull(error);
  g_clear_pointer(&error, g_free);

  memset(mask, 0, W * H * sizeof(float));
  mask[80 * W + 80] = NAN;
  g_assert_false(dt_magiceraser_nearby_fill(rgb, mask, W, H,
                                             proposal, &error));
  g_assert_nonnull(error);
  g_clear_pointer(&error, g_free);
  for(size_t i = 0; i < 3 * W * H; i++)
    g_assert_cmpfloat(proposal[i], ==, -1.0f);
  g_free(rgb);
  g_free(mask);
  g_free(proposal);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/magiceraser/result/clip", test_valid_clip_and_unblended_color);
  g_test_add_func("/magiceraser/result/layout", test_exact_origin_shape_and_layout);
  g_test_add_func("/magiceraser/result/invalid", test_invalid_values_are_transactional);
  g_test_add_func("/magiceraser/nearby/small", test_nearby_fill_small_painted_area);
  g_test_add_func("/magiceraser/nearby/refusal", test_nearby_fill_rejects_without_mutation);
  return g_test_run();
}

// clang-format off
// modelines: These editor modelines have been set by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
