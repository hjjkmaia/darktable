# The .dtdata sidecar

Per-pixel data that belongs to an edit but does not fit an XMP: raster
masks today, depth maps and painted layers later. Design discussion:
https://github.com/darktable-org/darktable/issues/22226

## File

`IMG_0001.RAF.dtdata` next to `IMG_0001.RAF.xmp`; duplicates get the same
`_NN` suffix as their XMP. A flat zip with no manifest:

```
version                 "1"
mask-<sha1>.png         8-bit gray, an alpha mask
depth-<sha1>.png        16-bit gray
brush-<sha1>.png        8-bit gray
patch-<sha1>.png        8- or 16-bit RGBA pixels for a cropped accepted fill
```

An entry is named by the SHA-1 of its bytes and is never modified. A new
mask is a new entry; history steps point at different ones. Readers
ignore entries they do not know, writers keep entries they cannot
attribute, so an older darktable never drops what a newer one wrote.
Nothing in the file describes an entry: that lives with the reference.

## Reference

`dt_dtdata_ref_t` in `src/common/dtdata.h` is the only thing stored in
history: entry name, kind, origin (regenerable by a model or made by a
person), size, bit depth, producer. It has no path. The entry is looked
up in the sidecar beside the XMP the history came from, by the same rule
that finds the XMP, so moving a folder breaks nothing. Like the XMP, the
sidecar sits beside the original when it is reachable and beside the
local copy otherwise. Creating a local copy copies the sidecar into the
cache with it, so masks imported while the original is offline land in
the cache sidecar and earlier ones stay readable; a read that finds no
entry beside the original falls back to the cache sidecar while the
image has a local copy. Resetting the local copy merges the cache
sidecar back into the original's and deletes it; if the merge fails the
local copy is kept whole for a retry. On read the SHA-1 is
checked against the name; a missing or damaged entry is an empty mask
and a log line.

The reference is copied verbatim wherever module params travel. The
first consumer, `src/iop/rasterfile.c`, lays the fields out inline in its
params so introspection can see them.

## API

`src/common/dtdata.c`, libarchive for the zip and libpng in memory for
the entries:

- `dt_dtdata_enabled()`: FALSE when sidecar writing is "never". No new
  entry is written then and there is no database fallback; existing
  sidecars are still read, and sweeps and merges still rewrite or delete
  them. The rasterfile module falls back to its previous behavior in
  that case, a file referenced by path inside the raster mask root
  folder
- `dt_dtdata_path()`, `dt_dtdata_path_for_image()`
- `dt_dtdata_write_gray()`, `dt_dtdata_read_gray()`, `dt_dtdata_list_entries()`
- `dt_dtdata_write_rgba()`, `dt_dtdata_read_rgba()`: cropped RGBA patch
  pixels plus alpha in one content-addressed entry. Accepted fills use
  authoritative references, so reopening never depends on the model.
  Patch RGB is encoded sRGB, independent of the preview or export profile;
  the Magic Eraser pixelpipe module converts it to the active output profile
  before blending. Alpha is coverage and is not color-managed. An inpainting
  proposal stores unblended replacement RGB and its fractional selection
  coverage; preview and saved rendering each apply that alpha exactly once.
  Decoding checks the checksum, PNG color/depth, and reference dimensions.
  The per-entry limit is 16 megapixels; callers must reject or split larger
  patches.
  Writing is unavailable when sidecar writing is "never".
- `dt_dtdata_source_sha256()`: fingerprint the bytes of the photo file that
  the image loader uses, including a local copy when present. Magic Eraser
  v3 history parameters store this digest alongside the geometry guard.
  An accepted patch is refused on a different photo or after the source
  bytes change; the path and catalog ID are deliberately not part of the
  fingerprint. Older v1/v2 prototype patches have no source attestation
  and fail closed while their sidecar entries remain recoverable.
- `dt_dtdata_merge()`: copy one image's entries into another's; called
  from `dt_history_copy_and_paste_on_image()` because a pasted reference
  needs its entry on the destination
- `dt_dtdata_register_scanner()`, `dt_dtdata_sweep()`:
  cleanup, see below
- `dt_dtdata_find_all()`: the sidecars of an image and its duplicates by
  name pattern, for the delete job, since an xmp may not exist yet
- `dt_dtdata_file_*`: the same on explicit paths, used by the cmocka test
  `src/tests/unittests/common/test_dtdata.c`

Writers rewrite the whole zip to a temp file and rename it, holding the
write side of a read-write lock; readers open the file per call under
the read side, since Windows refuses the rename while the file is open.
A zip with an incomplete central directory, a mismatch between its central
directory and streamed entries, an unreadable entry, or a known entry whose
bytes differ from the checksum in its name is never rewritten or merged.
Merge validates both sidecars, builds their union in a temporary zip, and
renames it once while retaining the destination file mode. A later damaged
entry cannot leave earlier entries half-merged. History paste checks its
known raster references and merges the entries before changing destination
history; a missing referenced entry cancels the paste. The rasterfile
module caches the decoded mask by its params hash,
and the SHA-1 in the entry name is part of those params, so the cache is
content-addressed for free.

## Cleanup

An entry is garbage when no row of the image's stored history references
it. That is decided only when the history is final: undo lives inside one
darkroom session and older history items can be clicked at any time, so
reset, instance delete and re-import never touch the file. The sweep runs
when the darkroom leaves or switches image, right after the history is
written, and from the lighttable "compress history" job, which has no
undo. "discard history" leaves the file alone, since the lighttable undo
can bring the history back; the sweep removes the orphaned entries, and
the file once it is empty, the next time the image is left in the darkroom
or compressed.

Modules are plugins, so core code cannot read their params. A producer
registers a scanner from `init_global()`, "given a params blob and its
version, which entry does it reference", together with the entry kinds it
writes. The sweep only removes entries of a registered kind, so a build
that lacks the producer of some kind leaves those entries alone.
`rasterfile` registers for the `mask` kind.

A scanner returning -1 cancels the sweep for that image, but that only
covers rows that reach the sweep unchanged. Known limitation: history
loading (`src/develop/develop.c`) replaces a row whose params version is
newer than the module knows with default params before any sweep runs,
as it does for every module, so an older build sweeps away masks that
only newer-version rows referenced.

## Lifecycle

The sidecar follows the XMP in `dt_image_rename()` (move and rename; a
sidecar that cannot be moved is copied, since its masks exist nowhere
else), both branches of the delete job, and local-copy reset. It is refused as
an importable file. History-preserving duplication and image copy temporarily
refuse a source with a `.dtdata` beside either the original or local copy,
including an unreferenced or damaged archive. Virgin duplicates still work.
Direct history paste into an existing image still merges sidecar entries
before applying history. This restriction remains until a failed sidecar copy
can roll back the new row and Undo record transactionally. The preflight does
not close the race if a sidecar appears between checking and copying. Not
handled yet: the
"write sidecar files" action (the file is written at import time, not synced
from the database), the
crawler, XMP-only transfer, and styles carrying a reference to an entry
the target image does not have.
