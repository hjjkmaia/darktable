/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

#pragma once

#include "develop/imageop.h"

/* Import some lightroom develop options
   When called from lightable : dev == NULL, in this case only the tags are imported
   When called from darkroom  : dev != NULL, in this case only develop data are imported
*/
gboolean dt_lightroom_import(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto);

/* Re-read the photo-flavor sidecar (IMG.xmp beside the raw, never
   IMG.ARW.xmp) and replace the history rows dt_lightroom_import()
   wrote. Preset rows and edits to modules the importer did not write
   stay. This does not clear DT_IMAGE_AUTO_PRESETS_APPLIED, so it does
   not re-apply presets and does not delete the rest of the history.

   A later UI calls this with the develop that already has the image
   open, the same dev the darkroom path passes to dt_lightroom_import().
   dev must be non-NULL. Pass iauto FALSE so a missing sidecar is
   reported. There is no button in this change:

     dt_lightroom_reimport(dev->image_storage.id, dev, FALSE);

   The history harness passes reimport=true to get_history. That opens
   the image and calls this. It is a test hook, not the UI.
*/
gboolean dt_lightroom_reimport(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto);

/* returns NULL if not found, or g_strdup'ed pathname, the caller should g_free it. */
char *dt_get_lightroom_xmp(dt_imgid_t imgid);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

