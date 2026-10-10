# Lightroom verification harness

Lightroom is the standard. This directory measures whether a darktable render
moves a raw the way a Lightroom slider should: same panel, same direction,
checked against the owner's photo-flavor sidecars (`IMG.xmp` beside `IMG.ARW`,
`crs:` namespace, Process Version 15.4). Pixel identity with Adobe's process
is not the goal. Scales Lightroom does not publish are calibrated here.

The client is Python 3, standard library only. It speaks JSON-RPC to this
worktree's `darktable-mcp`. It does not open `~/.config/darktable`.

## Baseline

The "no change" render is darktable's workflow-default render, not a flat
one. A fresh config uses `plugins/darkroom/workflow` =
`scene-referred (sigmoid)`. On a raw, that pipeline's exposure module is
the scene-referred default, **+0.7 EV** (`src/iop/exposure.c`,
`reload_defaults`). Replacing that default with exposure 0 makes the picture
darker than both Lightroom's Exposure 0 and darktable's normal starting point.

`schema --op exposure` reports the introspection default of that field, which
is 0. The rendered baseline is still +0.7 EV, because `reload_defaults`
replaces that 0 for a scene-referred raw before the history row is written.
Read `history`, or a no-stack `stats`, for the starting exposure.

`stats` and `render` with no `--stack` measure that baseline. `compare`
renders both stacks on top of it. `--no-tonemapper` switches the workflow
tone mapper off (sigmoid, filmic, AgX, spektrafilm, or basecurve) so a tone
mapper in the stack owns the curve. That is a different measurement, and the
JSON says so (`no-tonemapper` instead of `workflow-default`).

A stack entry's `params` set those fields after the workflow history has
been replayed. The numbers are absolute darktable values, not Lightroom
deltas. An exposure entry of `0` is exposure 0, which is darker than the
baseline. Lightroom Exposure 0 corresponds to leaving the +0.7 EV default
alone; a non-zero Lightroom Exposure is `0.7 + EV`.

## Server

The first command starts one server and later commands reuse it. Startup is
allowed 90 seconds. The process is:

```sh
export GSETTINGS_SCHEMA_DIR=/opt/homebrew/share/glib-2.0/schemas \
       XDG_DATA_DIRS=/opt/homebrew/share
/Users/mj/tools/darktable-grok/build/bin/darktable-mcp \
  --core --configdir /tmp/lr-harness/cfg \
         --cachedir /tmp/lr-harness/cache \
  --library :memory: --conf write_sidecar_files=never
```

The binary is this worktree's `build/bin/darktable-mcp` (override with
`DTMCP_BIN`). Protocol stdout is JSON-RPC. darktable's own log goes to
`/tmp/lr-harness/mcp.stderr`. The catalog is in memory, so nothing is written
to a library you keep, and sidecar writing is off.

Stop it with `kill "$(cat /tmp/lr-harness/mcp.pid)"`. The client restarts it
if the process dies or if `dtmcp.py` or the binary changes.

These Sony ARWs log a rawspeed warning,
`Unsupported photometric interpretation: 6`, and still render. The warning
goes to `/tmp/lr-harness/mcp.stderr`, not into the JSON.

The client refuses to open or write anything under `~/Desktop/sony`,
`~/photo-flavor`, or `~/.config/darktable`. Copy fixtures first.

## Commands

Run from anywhere:

```sh
tools/lr-harness/dtmcp.py tools
tools/lr-harness/dtmcp.py schema  --op exposure
tools/lr-harness/dtmcp.py stats   --raw /tmp/lr-fixtures/DSC04302.ARW --json
tools/lr-harness/dtmcp.py stats   --raw /tmp/lr-fixtures/DSC04302.ARW \
    --stack stack.json --width 512 --no-tonemapper --json
tools/lr-harness/dtmcp.py render  --raw /tmp/lr-fixtures/DSC04302.ARW \
    --out /tmp/preview.png
tools/lr-harness/dtmcp.py compare --raw /tmp/lr-fixtures/DSC04302.ARW \
    --stack A.json --stack B.json --json
tools/lr-harness/dtmcp.py history --raw /tmp/lr-fixtures/DSC04302.ARW
tools/lr-harness/fixtures.sh
```

`stats` defaults to a width of 512. The server fits the image inside that
width and keeps the aspect ratio. `compare` uses the same width, with the
tone mapper left on. `render` passes no size, so the server's own default
applies (a 1024×1024 box).

`stats` and `render` pass `input.path`. darktable-mcp imports that path for
the one call and then drops the row. The file on disk is not the catalog.

`--json` on `stats` and `compare` is the machine-readable form. Two runs of
the same command print the same JSON, including the decimals of `mean`.
`compare` reports, per channel, `min`, `max`, `mean`, `p1`, `p50`, `p99`,
`clip_lo`, and `clip_hi`. `delta` is stack B minus stack A. Those values
come from the 8-bit preview `image_stats` already computes.

### Stack JSON

A stack file is a JSON array of MCP stack entries, the same objects
`image_stats` and `render` take:

```json
[
  {"operation": "exposure", "params": {"exposure": 1.2}, "enabled": true}
]
```

A single entry object, or `{"stack": [ ... ]}`, is accepted too. Field
names and ranges come from `schema --op`. A number outside the module's
min/max is rejected by the server.

### `history`

`history` needs the photo-flavor sidecar `basename.xmp` beside the raw
(`DSC04302.ARW` needs `DSC04302.xmp`). It does not read `DSC04302.ARW.xmp`.

Each invocation does this:

1. Copy the raw and the photo-flavor sidecar to a new directory under
   `/tmp/lr-harness/history/`. The caller's file is only read. A new copy
   has never had auto-presets applied, so the import is the first open.
2. `import_images` on that copy. This is the library path (`dev == NULL`).
   It records rating, labels, tags, GPS, and metadata. It does not write
   module rows.
3. `get_history` on that `imgid`. The server is started with
   `DT_LR_HISTORY_OPEN=1`. Under that variable, and only then,
   `dt_bridge_get_history_json` runs a darkroom open before it reads the
   database (see below). Stdout is JSON: file name, sidecar name, the
   history entries (operation, version, enabled, iop order, and decoded
   fields when the blob matches the module's current layout), and
   `second_open_added`.
4. `get_history` again on the same `imgid`. That is a second open.
   `second_open_added` is how many rows it appended. The import is
   one-shot, so the number is 0.

`image_stats` is not part of this sequence. Export loads the image with
`dt_dev_init(FALSE)`, so `full.pipe` stays NULL, `dt_lightroom_import` does
not write module rows, and `first_run` is still consumed. Calling it before
`get_history` would burn the only import.

#### How the darkroom open runs headless

Module rows are written only on the darkroom path, and only when
`dev->full.pipe` is loading (`dt_dev_read_history_ext` in
`src/develop/develop.c`). `dt_dev_init` creates that pipe only when its
`gui_attached` argument is true. MCP export and `darktable-cli` call
`dt_dev_init(FALSE)`, so the pipe stays NULL and a first open through them
records the workflow defaults only.

`dt_dev_image()` already has the headless shape of a darkroom open: it
calls `dt_dev_init(TRUE)` (the pipe exists) and then sets `gui_attached` to
false so nothing talks to widgets. `dt_dev_load_image()` then sets
`full.pipe->loading` and calls `dt_dev_read_history_ext`. On `first_run`
that calls `dt_lightroom_import`.

`history` uses that sequence and does not render. In
`src/mcp/dt_bridge.c`, `_darkroom_open()` is:

```c
dt_dev_init(&dev, TRUE);
dev.gui_attached = FALSE;
dt_dev_load_image(&dev, imgid);
dt_dev_cleanup(&dev);
```

`dt_bridge_get_history_json` calls it when `DT_LR_HISTORY_OPEN` is set and
the server is not `--read-only`, then queries `main.history`. The harness
sets the variable in the server environment. Other MCP clients that do not
set it still get a plain history read. The harness also drops
`DT_LR_MAP_VALIDATE` from that environment: when it is set, `darktable-mcp`
prints the mapping self-check and exits instead of serving. This harness
does not launch the GUI.

`darktable-cli` was the other candidate. It uses the same headless
`dt_dev_init(FALSE)` as MCP's export, and its `--verbose` history print
runs before that load, while the default library is `:memory:` and
disappears when the process exits.

The workflow-default rows are still there (exposure +0.7 EV, sigmoid, and
the rest of the auto-applied stack). Mapped Lightroom tags append one later
row per module at `multi_priority` 0. For an equal priority the later row
is the one the pipe uses. `iop_order` in the JSON is the integer
`get_history` returns (a pipeline slot, with gaps of 100), not the legacy
float in `src/common/iop_order.c` (sigmoid is 45.3 there and 6200 here).
The scratch copy grows no `DSC04302.ARW.xmp`.

`history --reimport` does that sequence, then rewrites
`ParametricHighlights` to 40 on the scratch sidecar only and calls
`get_history` with `reimport: true`. The server opens the image and calls
`dt_lightroom_reimport()`. The JSON gains `reimport_history`,
`reimport_highlights`, and `reimport_added` (rows after re-import minus
rows after the first open). A UI calls the same function with the open
develop; see `dt_lightroom_reimport()` in `src/develop/lightroom.h`.
There is no darkroom button.

Running `history` twice compares two first opens of two scratch copies. The
`second_open_added` field is the second develop of one image.

## Fixtures

`fixtures.sh` copies into `/tmp/lr-fixtures/` and nowhere else:

| File | Source | Role |
|---|---|---|
| `DSC04299.ARW`, `DSC04300.ARW`, `DSC04302.ARW` | `~/Desktop/sony/11760513/` | raws |
| `DSC04299.xmp`, `DSC04300.xmp`, `DSC04302.xmp` | the photo-flavor `IMG.xmp` next to those raws, not `IMG.ARW.xmp` | owner's sidecars (Moody Teal-Orange Matte Base). Parametric sliders are identity |
| `cinematic-teal-orange.xmp` | `~/photo-flavor/samples/` | contrast +22, texture +8, dehaze +8, grade luminance off zero (shadow −5, midtone +2, highlight +5) |
| `parametric-identity.xmp` | a copy of `DSC04299.xmp` | named regression sidecar: `ParametricShadows/Darks/Lights/Highlights` are 0 and the splits are 25/50/75 |

Every photo-flavor sidecar under `~/photo-flavor` has identity Parametric
tags, so the moving sample and the identity sidecar are different files on
purpose. `cinematic-teal-orange.xmp` has no raw of its own. To run `history`
on it, copy a fixture raw to a scratch name that matches:

```sh
mkdir -p /tmp/lr-pair
cp /tmp/lr-fixtures/DSC04302.ARW /tmp/lr-pair/cinematic-teal-orange.ARW
cp /tmp/lr-fixtures/cinematic-teal-orange.xmp /tmp/lr-pair/
tools/lr-harness/dtmcp.py history --raw /tmp/lr-pair/cinematic-teal-orange.ARW
```

## Scratch rules

- Fixture copies live in `/tmp/lr-fixtures/`. Commands take those paths.
- Server config and cache live in `/tmp/lr-harness/`. The catalog is
  `:memory:`.
- `history` copies again under `/tmp/lr-harness/history/` before import.
- `stats` and `render` use a scratch `input.path`, which the server drops
  before the call returns.
- No sidecar is written next to a raw (`write_sidecar_files=never`).
