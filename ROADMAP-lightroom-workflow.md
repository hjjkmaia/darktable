# Lightroom-style editing for this darktable fork: roadmap and handoff

This file is the single source of truth for the "Lightroom workflow" work on
this fork. It is written so that any agent (or person) can take over without
having seen the conversation that produced it. Read it top to bottom once.

- Fork owner: matt j (`13343710@qq.com`)
- Branch with this work: `feature/lightroom-workflow`
- Last updated: 2026-10-10

This is a fork-only document. When anything from this branch is proposed
upstream, drop the commit that adds this file (see "Upstreaming" below).

---

## 0. Take over in five minutes

```bash
cd ~/tools/darktable
git status -sb                      # expect: ## feature/lightroom-workflow
git log --oneline -5                # see "Status board" for what each commit is
cmake --build build                 # incremental build, ~5 s after small edits
# quit any running darktable GUI first (Cmd+Q in the darktable window), then:
cmake --install build               # installs into build/macosx
~/tools/darktable/restart-dev.sh            # one Terminal window; ~40 s to start
```

Then read: section 6 (status board), `ARCHITECTURE-lightroom-workflow.md`
(the frozen contract for the next stages), and the section of the phase you
are picking up. The upstream rules in `AGENTS.md` apply too;
`AGENTS.local.md` (git-ignored) has the machine-specific notes.

---

## 1. Goal

The owner wants darktable to work like Adobe Lightroom Classic's Develop
module, starting with the routine edits in Lightroom's order:

1. exposure
2. saturation (Lightroom: vibrance, then saturation)
3. tone curve

and later the rest of Lightroom's Develop panels.

Key design decision (made with the owner): **do not write new image
processing.** darktable already has every processing step. What is missing is
a Lightroom-shaped *workflow layer*: one panel, Lightroom's sliders, in
Lightroom's order, driving darktable's existing modules (`exposure`,
`colorbalancergb`, `rgbcurve`, ...).

UI order is not processing order. Lightroom applies edits in a fixed internal
order no matter which slider you move first; darktable's pixelpipe order is
also fixed. Only the order in which controls are *presented* changes.

The owner's photos: Sony A1 II (ILCE-1M2), lossless compressed RAW at the
M/S size, e.g. `~/Desktop/sony/11760513/` (3,442 ARW + camera JPEG pairs).
Making those files open at all was Phase 0.

---

## 2. Where everything is

### Machines

- **This Mac (primary, `mj`)**: Apple Silicon, macOS 26.5, 18 cores.
  All work happens here.
- **Old MacBook (`hjjk@hjjk.local`, SSH key auth)**: had the original shallow
  clone at `/Users/hjjk/tools/darktable` (branch `feature/export-enhancements`,
  untouched). It is no longer needed and may be powered off.

### Paths on this Mac

| What | Path |
|---|---|
| repo (main worktree) | `~/tools/darktable` |
| build tree | `~/tools/darktable/build` (Ninja, Release) |
| installed app | `~/tools/darktable/build/macosx` (`bin/darktable`, `bin/darktable-cli`) |
| launcher | `~/tools/darktable/restart-dev.sh` (reuses one Terminal window; git-ignored, as is `run-dev.sh`) |
| logs, scratch notes, agent task briefs | `~/tools/darktable/.local-logs/` (git-ignored) |
| Kimi's read-only worktree | `~/tools/darktable-kimi` (detached HEAD) |
| darktable config/library on this Mac | `~/.config/darktable/` |
| config backup from the old Mac | `~/tools/darktable-config-from-hjjk/` (do not install over the live config) |
| owner's test photos | `~/Desktop/sony/11760513/` (do not modify; see 9.4) |
| scratch test area | `/tmp/dtcli-test/` (safe to delete) |

### Git

- `origin` = `https://github.com/darktable-org/darktable.git`, partial clone
  (`--filter=blob:none`): full history, blobs fetched on demand.
- There is **no personal remote yet**. The work exists only on this Mac
  (and the pre-rebase copy on the old Mac). Creating a GitHub fork or a
  Cursor-hosted repo is a pending decision for the owner.
- Branches:
  - `feature/lightroom-workflow`: this work (current)
  - `feature/export-enhancements`: the owner's April commit, rebased onto
    upstream master of 2026-10-07 (`8c64bf7ca`)
  - `backup/export-enhancements-pre-rebase`: the April commit before the
    rebase, as copied from the old Mac
  - `master`: tracks `origin/master`
- Submodules initialised: `rawspeed`, `LibRaw`, `OpenCL`, `libxcf`,
  `whereami`, `lua-scripts`. **Not** initialised: `src/tests/integration`
  (large test-image repo; needed only for the integration test suite).

---

## 3. Environment and build

### 3.1 Dependencies (Homebrew)

Installed for this build:

```
coreutils cmake adwaita-icon-theme desktop-file-utils exiv2 gmic gphoto2
gtk+3 gtk-mac-integration intltool iso-codes jpeg-xl json-glib lensfun
libarchive libavif libraw librsvg libsecret libsoup lua@5.4 ninja openexr
osm-gps-map portmidi potrace pugixml shared-mime-info imagemagick
```

Deliberately skipped: `onnxruntime` (only for `USE_AI`, off), `opencv` (only
HDR bracket auto-alignment), `graphicsmagick` (off; ImageMagick is used),
Homebrew `git`/`curl`/`perl` (system ones are fine).

Do **not** run `packaging/macosx/1_install_hb_dependencies.sh`: it runs
`brew update && brew upgrade`, which upgrades every Homebrew package on the
machine, not just darktable's.

### 3.2 Configure (already done; repeat only after deleting `build/`)

`build.sh` cannot pass flags that contain spaces, so cmake is driven directly:

```bash
cd ~/tools/darktable
P=$(brew --prefix libomp)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/macosx" \
  -DUSE_GRAPHICSMAGICK=OFF -DUSE_IMAGEMAGICK=ON \
  -DBUILD_CURVE_TOOLS=ON -DBUILD_NOISE_TOOLS=ON \
  "-DOpenMP_C_FLAGS=-Xpreprocessor -fopenmp -I$P/include" \
  "-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp -I$P/include" \
  -DOpenMP_C_LIB_NAMES=omp -DOpenMP_CXX_LIB_NAMES=omp \
  "-DOpenMP_omp_LIBRARY=$P/lib/libomp.dylib" \
  -Djsonschema_BIN=jsonschema_BIN-NOTFOUND
```

Why the odd flags:
- `libomp` is keg-only in Homebrew; pointing cmake at it avoids
  `brew link --force libomp` system-wide.
- `jsonschema_BIN-NOTFOUND`: the Homebrew `jsonschema` on this Mac is broken
  (its shebang points at a removed Python 3.13). This only skips the
  validation of `noiseprofiles.json` / `wb_presets.json`.
- "Could NOT find LLVM" during configure is harmless (only OpenCL kernel
  checks at build time).

Resulting feature set (`darktable --version`): OpenMP, OpenCL, Lua, gPhoto2,
OSMGpsMap, GMIC, ImageMagick, AVIF, HEIF, JXL, LibRaw 0.22, OpenJPEG,
OpenEXR, WebP enabled; OpenCV, GraphicsMagick, colord, AI disabled.

### 3.3 Build, install, run

```bash
cmake --build build                 # always build before claiming anything works
cmake --install build               # only with the GUI closed (see below)
~/tools/darktable/restart-dev.sh
```

- **Close the GUI before installing.** `cmake --install` overwrites
  libraries the running app has mapped; that can crash it. Quit darktable
  with **Cmd+Q** in its window (Ctrl+Q does nothing on macOS).
- **Launch with `restart-dev.sh`.** A GUI started in the background from an
  agent's shell is killed when that shell command ends, so the script still
  opens Terminal. It closes finished `run-dev.sh` windows first and does
  nothing if this dev darktable is already running. Do not use
  `open -a Terminal` or `do script` for this; each of those leaves the
  previous window open.
- **Startup takes about 40 s** on this Mac: each of the 97 plugin modules
  takes ~0.35 s to load (`-d common` shows `[iop_load_module]` lines). It is
  not a hang. (Unconfirmed guess: macOS scanning freshly built, unsigned
  plugins; it may speed up on later launches.)
- `run-dev.sh` sets `GSETTINGS_SCHEMA_DIR` and `XDG_DATA_DIRS` to Homebrew's
  share dir and execs `build/macosx/bin/darktable` relative to itself.
- A `GLib-GObject-CRITICAL ... proxy-resolver` line at startup is harmless.

### 3.4 Testing without installing (preferred for loader/pipeline work)

The build tree binaries work uninstalled if given the module and data dirs:

```bash
cd ~/tools/darktable
export GSETTINGS_SCHEMA_DIR=/opt/homebrew/share/glib-2.0/schemas XDG_DATA_DIRS=/opt/homebrew/share
build/bin/darktable-cli INPUT.ARW OUT.jpg --width 1600 \
  --core --configdir /tmp/dtcli-test/cfg \
  --moduledir $PWD/build/lib/darktable --datadir $PWD/build/share/darktable \
  -d imageio
```

Traps:
- **darktable-cli reads `INPUT.ARW.xmp` if it exists**, including its history.
  To test the loader or default processing, copy the RAW to a folder without
  a sidecar first. (This cost an hour in Phase 0: a stale sidecar made a
  correct loader look broken.)
- darktable-cli never overwrites: an existing `OUT.jpg` makes it write
  `OUT_01.jpg`, `OUT_02.jpg`, ... Check the newest file, or clear the folder.
- Use a throwaway `--configdir` so tests never touch `~/.config/darktable`.

Pipeline debugging that worked well:

```bash
build/bin/darktable-cli IN OUT.jpg ... --disable-opencl \
  --dump-pipe rawprepare,temperature,highlights,colorin,exposure,channelmixerrgb,sigmoid,colorout \
  --dumpdir /tmp/dtcli-test/dump
python3 /tmp/dtcli-test/ppmstats.py /tmp/dtcli-test/dump/export   # per-module channel means
```

The dumps are 16-bit PPMs clamped to [0,1] (`src/common/pfm.c`
`_swap_float`), so a mean of exactly 1.0 means "clipped", not "white".
`ppmstats.py` and `probe.c` (a standalone LibRaw probe, see 7.3) are in
`/tmp/dtcli-test/`; copy them into `.local-logs/` if you want to keep them.

### 3.5 Regression samples

Public samples from raw.pixls.us (CC0), downloaded to
`/tmp/dtcli-test/reg/raw/` (re-download with `curl -L -O` if `/tmp` was
cleared):

- `https://raw.pixls.us/data/Sony/ILCE-7M3/_DSC0009.ARW` (Bayer ARW)
- `https://raw.pixls.us/data/Sony/ILCE-1/A1_full_lossless_compressed.ARW`
  (Bayer, full-size lossless: must stay on RawSpeed)
- `https://raw.pixls.us/data/Canon/EOS%20R/Canon_EOS_R_CRAW_ISO_100.CR3`
  (LibRaw path)

Old-vs-new comparison: export each file with the installed build (old) and
the build-tree binary (new), then `magick compare -metric AE old.png new.png
null:` (0 = pixel-identical). The exact loop is in the Phase 0 record (7.4).

### 3.6 GUI automation (screenshots and clicks)

- Screenshots: `peekaboo image --mode screen --path /tmp/x.png`. Cursor has
  Screen Recording permission now.
- Clicks: `peekaboo click --coords X,Y` in **screen points**. This Mac's
  screen is 1728x1117 points. If your image viewer shows the screenshot
  scaled (e.g. 1024 px wide), multiply coordinates back (x1.6875 for 1024).
- Bring darktable forward first:
  `osascript -e 'tell application "System Events" to set frontmost of (first process whose unix id is PID) to true'`
- **Check that nobody is using the screen.** The owner works on this Mac.
  If another app is in front, your clicks land in it. Stop and report
  rather than clicking blind.
- darktable's "act on" logic: in the lighttable *culling* layout, history
  buttons act on the image under the pointer, not the selection. Use the
  filemanager (grid) layout for multi-image actions.

---

## 4. Conventions for this branch

- Upstream `AGENTS.md` applies in full; read it. Highlights: every commit
  must compile; American English; comments and UI strings start lowercase
  with no full stop; ASCII punctuation; `DT_OMP_FOR` not bare pragmas;
  file-local functions are `static _name`; comment the why, not the what;
  don't reformat untouched code.
- darktable APIs that changed upstream recently (use the new ones):
  `DT_ENTER_GUI_UPDATE()` / `DT_LEAVE_GUI_UPDATE()` / `DT_GUARD_GUI_UPDATE()`
  instead of touching `darktable.gui->reset`; `dt_gdk_event_get_state(event)`
  instead of `event->state`; event controllers (`dt_gui_connect_click`,
  `dt_gui_connect_key`) instead of `"button-press-event"` signals.
- One commit per phase, each building on its own. The owner has asked for
  commits on this branch (AGENTS.md's "humans commit" default is overridden
  here by the owner).
- Keep the roadmap commit (`docs: add Lightroom workflow roadmap ...`) as the
  **last** commit of the branch and update it with `git commit --amend`, so
  editing the roadmap never changes the ids of the code commits. New phase
  commits go *below* it: commit the phase, then move the docs commit back to
  the end with the helper script
  `GIT_SEQUENCE_EDITOR=.local-logs/move-docs-last.sh git rebase -i <last code commit before docs>`.
  The script moves the docs `pick` line last and aborts the rebase if it
  cannot find it. Recreate it if `.local-logs/` was cleaned:

  ```sh
  #!/bin/sh
  f="$1"
  pattern='^pick [0-9a-f]+ (# )?docs: add Lightroom workflow roadmap'
  docs=$(grep -E "$pattern" "$f") || { echo "roadmap commit not found" >&2; exit 1; }
  grep -vE "$pattern" "$f" | awk -v d="$docs" '
    !done && $0 !~ /^(pick|exec|fixup|squash|drop|edit|reword|p|x|f|s|d|e|r) / { print d; done = 1 }
    { print }
    END { if(!done) print d }' > "$f.new" && mv "$f.new" "$f"
  ```

  Note that git 2.50 writes todo lines as `pick <sha> # <subject>`. Two
  scripting mistakes already happened here: `perl -i ... END{print}` sends
  the moved line to stdout and silently drops the commit (recovered from
  `git reflog`), and a pattern without the optional `# ` matched nothing.
- `.githooks/pre-commit` prints `declare: -A: invalid option` errors: it needs
  bash 4+, macOS ships bash 3.2. The commit still succeeds. Leave the hook
  alone.
- Commit identity on this Mac is already `matt j <13343710@qq.com>`.

### Rebasing onto upstream later

```bash
git fetch origin
git rebase origin/master            # conflicts so far were in src/gui/gtk.c
git submodule update --init --filter=blob:none -- src/external/rawspeed src/external/LibRaw \
  src/external/OpenCL src/external/libxcf src/external/whereami src/external/lua-scripts
cmake --build build
```

Re-run the Phase 0 regression comparison after any rebase that touches
`src/imageio/` or the bundled LibRaw/RawSpeed.

---

## 5. Agents and collaboration

- **Kimi Code** CLI is installed: `~/.kimi-code/bin/kimi` (v2.1.1, default
  model `kimi-code/k3`). Non-interactive use:
  `kimi -p "<task>" > out.md 2>&1` (run inside the directory it should
  read). `-p` cannot be combined with `--auto` (Kimi exits with "Cannot
  combine --prompt with --auto"); in `-p` mode it still reads files and runs
  commands such as `git show`, so state in the brief whether it may edit.
  A trivial prompt takes ~70 s; give it substantial, self-contained tasks.
- Give other agents **their own worktree** so they cannot disturb the main
  tree or its build: `git worktree add --detach ~/tools/darktable-<name> HEAD`.
  For implementation work by another agent, create a branch in that worktree
  and its own build dir (`cmake -S . -B build ...` from 3.2, ~5 min full
  build), then merge or cherry-pick.
- Task briefs and outputs live in `.local-logs/kimi-task-*.md` and
  `.local-logs/kimi-*.out.md`. Finished Kimi tasks (both high quality, every
  claim cited a file:line; spot checks held up):
  - review of the Phase 0 commit: `kimi-review-phase0.out.md` (see 7.6)
  - Lightroom Basic slider mapping: `kimi-lr-mapping.out.md` (see 10.2)
- Treat other agents' findings as leads: verify each claim against the code
  before acting on it.

### 5.1 Orchestration model (in use since Phase 1)

To keep cost down, implementation is delegated to Kimi; the orchestrating
agent briefs, reviews and verifies:

1. orchestrator writes a brief in `.local-logs/kimi-task-<name>.md`: scope,
   the roadmap section that specifies it, files allowed to change, build and
   warning check, "do not install / do not launch the GUI / do not push",
   commit message rules, and the report format
2. Kimi works in its own worktree and branch with its own configured
   `build/` (full build ~25 s, incremental seconds), runs
   `kimi -p "$(cat brief)"`, and commits once when it builds cleanly
3. orchestrator reviews the diff line by line against the spec and
   AGENTS.md, rebuilds, runs the checks that need the screen or the owner's
   photos (Kimi does not), then cherry-picks the commit onto
   `feature/lightroom-workflow`
4. the roadmap's status board is updated in the same step

Worktrees in use:

| Worktree | Branch / HEAD | Use |
|---|---|---|
| `~/tools/darktable` | `feature/lightroom-workflow` | main tree, orchestrator only |
| `~/tools/darktable-kimi` | detached, pre-rebase Phase 0 commit | Kimi read-only research; LibRaw submodule not checked out |
| `~/tools/darktable-kimi-impl` | `kimi/phase2-3` (from `0814f3cf6`), own `build/` | Kimi implementation; `kimi/phase1-qap-order` and `kimi/phase2-3` are both done and cherry-picked. For the next task create a new branch from the current `feature/lightroom-workflow` tip in this worktree (`git switch -c kimi/<task> feature/lightroom-workflow`) |

Kimi's branches start from a snapshot of `feature/lightroom-workflow`, so
bring work over with `git cherry-pick <commits>` onto
`feature/lightroom-workflow`, then move the roadmap commit back to the end
(see section 4). Remove a finished worktree with `git worktree remove <path>`.

---

## 6. Status board

| Item | State | Commit |
|---|---|---|
| Find the project, move it to this Mac, rebase onto upstream 2026-10-07 | done | `3267ea441` (rebased April commit) |
| Export enhancements (quick presets, limit-by, live aspect preview, film strip hidden by default, wheel-scrolls side panels) | done (owner's April work, verified in GUI after rebase) | `3267ea441` |
| Phase 0: open Sony A1 II lossless YCbCr ARWs | done, verified with darktable-cli; GUI thumbnail render verified; reviewed by Kimi (7.6) | `e3ab18a01` |
| Phase 0 leftover: reset stale history of `DSC04299`, `DSC04300`, `DSC04309` | done 2026-10-10 (all three `history_end="0"`) | - |
| Phase 0 leftover: open a Sony file in the darkroom view | done: `DSC04309` renders correctly | - |
| Phase 1: Lightroom-ordered quick access panel (exposure, vibrance/saturation, curve) | done: implemented by Kimi, one review fix by the orchestrator, verified on screen (9.5) | `69abab414` |
| Phase 2: complete Lightroom Basic panel | done: implemented by Kimi, reviewed, verified on screen (10.5) | `cdef3a297` |
| Phase 3: Lightroom panel tabs | done: implemented by Kimi, reviewed, verified on screen (11.1) | `9a9f4add5` |
| Architecture v1 (F1/F2/F3 + A/B/C/D) | **approved 2026-10-10**. Lightroom is the standard (section 1 of the architecture). HSL targets `colorequal` (D9) | this docs commit |
| Stage 1: F2 params builder + F1 mapping table | landed, reviewed | `568001232`, `642786bfc` |
| Stage 1: F3 verification harness | landed, reviewed. Headless `history` dumps workflow defaults only; Lightroom module rows need a darkroom-attached open | `f813a93aa` |
| Stage M: Lightroom-style masks | **M1 and M2 landed.** Panel lists adjustments and sliders; drawing uses the existing mask manager after focus. On-screen check still needed | `92edbe0af`, `0e6e8e3e3` |
| Stage 3: parametric switch, re-import, detail | landed. Parametric highlights 100 becomes `paramcurve` 1.0. Re-import replaces `lr-import` rows. Sharpness → `sharpen`, NR → `nlmeans` because `denoiseprofile` is not auto-applied and has no luma/chroma fields. D Auto tone still waiting on a screen check of C | `8130ff879`, `91b9ed6de`, `10deb2424` |
| C before/after | landed. `\` shows the first-open prefix (exposure +0.7 and the tone mapper), not an empty history. On-screen toggle still needs a look | `82dd9a35f` |
| Stage 2: B Lightroom XMP import | landed. D1–D10 and D14. White balance adds (Temperature - 5500) K to the as-shot CCT, with a small tint, on color calibration. Headless `history` now opens the darkroom path. Negative Blacks can fall outside sigmoid's black-target range and are left at the default | `4840fc098`, `a42a9ca52`, `1022882cc`, `d72eb3ee8a` |
| Stage 2: A parametric curve | landed. Peak is the largest monotonic value (~0.016); calibrate against Lightroom later | `312e3f229` |
| Stage 4: integration, verification matrix, docs | waiting | - |
| Remaining backlog (section 12) | not scheduled | - |

---

## 7. Phase 0 record: Sony A1 II lossless YCbCr ARWs (done)

### 7.1 Problem

`~/Desktop/sony/11760513/*.ARW` from the ILCE-1M2, 4608x3072 (about 14 MP,
the "M" lossless size), could not be opened or exported at all. Thumbnails
showed only because darktable falls back to the embedded JPEG preview.

### 7.2 Root cause

1. The files store tiled lossless JPEG with **YCbCr** data
   (`PhotometricInterpretation = 6`, `Compression = 7`, 3 samples). This is
   already-demosaiced camera RGB, not a Bayer mosaic. RawSpeed only accepts
   CFA data there: `ArwDecoder.cpp:307` throws
   `Unsupported photometric interpretation: 6`.
2. darktable maps that exception to `DT_IMAGEIO_UNSUPPORTED_FEATURE`
   (`imageio_rawspeed.cc`, the `RawDecoderException` catch: any message
   containing "supported"). `_image_handled()` in `imageio.c` counts that code
   as handled, so the fallback chain stopped before LibRaw.
3. LibRaw 0.22 (bundled) *does* decode these (`sony_ycbcr_load_raw()` in
   `src/external/LibRaw/src/decoders/sonycc.cpp`, output in
   `rawdata.color4_image`), but darktable's LibRaw loader only took CR3/X3F
   and only accepted Bayer `raw_image` data.
4. The in-camera as-shot white balance is **baked into** the YCbCr data
   (`as_shot_wb_applied = LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_SONY` in
   `LibRaw/src/utils/open.cpp`; LibRaw divides `cam_mul` back out for
   daylight WB in `postprocessing_utils_dcrdefs.cpp`). darktable applies white
   balance itself, so it must be divided out at load.

### 7.3 Fix (commit `e3ab18a01`)

- `src/imageio/imageio.c`: ARW gets its own magic-number loader `_open_arw()`
  (line ~609, used by both ARW signatures at lines ~343/345). It calls the
  shared TIFF/RawSpeed loader and, only when that returns
  `DT_IMAGEIO_UNSUPPORTED_FEATURE`, tries `dt_imageio_open_libraw()`. A LibRaw
  failure keeps RawSpeed's original return code. NEF/ERF/PEF/SRW are
  untouched (they share the old loader).
- `src/imageio/imageio_libraw.c`:
  - `_is_arw()` (line ~333) and `_is_sony_ycbcr()` (line ~341, checks
    `libraw_get_decoder_info()` reports `sony_ycbcr_load_raw()`).
  - `dt_imageio_open_libraw()` (line ~452): non-whitelisted ARWs are opened
    with LibRaw only as a probe; anything that is not Sony YCbCr returns
    `DT_IMAGEIO_LOAD_FAILED` exactly as before (line ~479).
  - `_open_sony_ycbcr()` (line ~351): loads the image as an sRAW like
    `dt_imageio_open_rawspeed_sraw()` in `imageio_rawspeed.cc`:
    `DT_IMAGE_S_RAW`, 4-channel float, `filters = 0`, `IOP_CS_RGB` (as
    `imageio_rawspeed.cc:609` does for sRAWs); pixels normalized with
    LibRaw's `black` (1024) and `maximum` (17536) and divided by
    `cam_mul[c] / cam_mul[1]` (skipped if either is not positive);
    `raw_black_level = 0`, `raw_white_point = 65535` so `rawprepare` leaves
    the data unscaled.
  - Shared metadata code (WB coeffs = `cam_mul`, `adobe_XYZ_to_CAM` from
    LibRaw's `cam_xyz`, crop from LibRaw sizes) runs before the branch at
    line ~551.

Standalone LibRaw probe used to establish the numbers (`/tmp/dtcli-test/probe.c`),
build with:

```bash
clang -O1 -I ~/tools/darktable/src/external/LibRaw probe.c \
  ~/tools/darktable/build/bin/external/LibRaw-cmake/libraw.a \
  -L$(brew --prefix libomp)/lib -lomp -ljpeg -L/opt/homebrew/lib -lz -llcms2 -lc++ -o probe
./probe FILE.ARW
```

For `DSC04309.ARW` it printed: decoder `sony_ycbcr_load_raw()`, flags
`0x400` (LEGACY_WITH_MARGINS), raw 4608x3072, pitch 36864, visible size
4332x2892, black 1024, maximum 17536, cam_mul 2250/1024/1852, centre pixel
2234/2243/2286 (neutral gray: confirms the baked-in WB).

### 7.4 Verification done

- `DSC04309.ARW` (sidecar-free copy) exports and matches the camera JPEG in
  framing, white balance and color; darktable's default look is slightly
  less saturated, as expected.
- Five more files spread across the folder (`DSC04666`, `DSC05402`,
  `DSC06137`, `DSC06877`, `DSC07614`) export without errors; contact sheet
  `/tmp/dtcli-test/a1/sheet.jpg` (camera left, darktable right; portrait
  camera thumbnails look sideways only because `sips` ignored EXIF rotation).
- Regression, old build vs new build, 0 differing pixels for all three
  samples in 3.5. Bayer ARWs never reach LibRaw; CR3 takes the LibRaw path as
  before.
- In the GUI, `DSC04300`'s thumbnail was re-rendered from the RAW after its
  history was reset.

### 7.5 Known limitations and follow-ups

- `maximum = 17536` is LibRaw's estimate from an over-exposed ISO 100 frame
  (see `sonycc.cpp`); highlight clipping may be slightly off on other ISOs.
- Highlight reconstruction behaves like other sRAWs (no CFA data).
- A native RawSpeed implementation would be the "proper" upstream fix
  (RawSpeed would need a Sony YCbCr LJpeg decoder plus a YCbCr-to-RGB step,
  like its Canon sRAW interpolator). Not planned.
### 7.6 Independent review (Kimi, 2026-10-10)

Full report: `.local-logs/kimi-review-phase0.out.md`. It verified the
buffer indexing against LibRaw's source (`unpack.cpp`, `decoder_info.cpp`),
the white-balance round trip through `temperature.c`, the fallback return
codes and the OpenMP loop: no bugs. Its nits and what was done:

- `IOP_CS_RAW` differed from the RawSpeed sRAW path: changed to `IOP_CS_RGB`.
  Exports before and after are pixel-identical.
- a zero `cam_mul[1]` would zero all channels: guarded in the Sony path only
  (the shared `cam_mul[0]` check is unchanged, so CR3 behaviour is too).
- the decoder-name string must match LibRaw: comment added.
- the fallback is keyed on the `.arw` extension: left as is (safe; renamed
  files never loaded before either).
- pre-existing upstream issue, not fixed (per AGENTS.md): `imageio.c` calls
  `dt_imageio_open_libraw()` without `#ifdef HAVE_LIBRAW` and there is no
  stub, so `-DUSE_LIBRAW=OFF` builds were already broken before this work.

---

## 8. Background: how darktable's quick access panel works

Read this before Phase 1. Everything is in `src/libs/modulegroups.c`
(line numbers as of commit `e3ab18a01`).

- A **module-group preset** defines (a) the quick access panel (QAP) item
  list and (b) the module tabs. Built-in presets are created on every start
  in `init_presets()` (line ~1676) with `dt_lib_presets_add(...)`; they are
  read-only.
- Macros (lines ~1605-1674): `SQA(is_scene_referred)` starts a preset string
  with the default QAP items; `SNQA()` starts one with no QAP; `SMG(name,
  icon)` starts a module tab; `AM(x)` adds a QAP item (when used before the
  first `SMG`) or a module (after an `SMG`).
- Preset string format (`_preset_to_string()` line ~1473,
  `_preset_from_string()` line ~1507): sections separated by `ꬹ`.
  - section 0: `show_search|full_active`
  - section 1 (QAP): `basics_show|<reserved>|<reserved>|item|item|...`.
    The writer emits `"ꬹ%d||"`, so fields 1 and 2 are empty; the reader
    starts items at index 3 and ignores fields 1 and 2.
  - sections 2+: `group name|icon|<reserved>|module|module|...`
- QAP item ids are darktable action paths: `module_op/widget` or
  `module_op/section/widget` (e.g. `agx/curve/contrast`), or just
  `module_op` for the module's on/off button. Parsed in `_basics_init_item()`
  (line ~301). Widget names come from the module GUI:
  `dt_bauhaus_*_from_params(self, "field")` uses the param's `$DESCRIPTION`
  (e.g. `colorbalancergb.c:98` "global vibrance"), and
  `dt_action_define_iop(self, section, N_("name"), widget, ...)` for graphs
  and notebooks (e.g. `rgbcurve.c:1522` "curve", `rgbcurve.c:1486` "channel").
- Building the QAP: `_basics_show()` (line ~775) walks
  `darktable.develop->iop` **from the last module to the first** (reverse
  pipeline order) and, inside each module, adds items in the module's own GUI
  order via `_basics_add_items_from_module_widget()` (line ~719). That is why
  the panel cannot follow Lightroom's order today.
- `_basics_add_widget()` (line ~467) *reparents the module's real widget*
  into the QAP (a placeholder label keeps its slot in the module) and, when
  the position argument is not `NORMAL`, first creates a module header box
  (`FIRST_MODULE` / `NEW_MODULE`). It accepts only widgets whose parent is a
  `GtkBox` or `GtkGrid`.
- The preset editor (`_manage_editor_basics_update_list()`, line ~2131)
  *displays* QAP items in pipeline order, but saving writes `d->edit_basics`
  in stored order, so a stored order survives edit and duplicate.
- Group icons available (`_buttons_get_icon_fct()`, line ~244): `basic`,
  `tone`, `color`, `correct`, `effect`, `grading`, `technical`, plus
  `active` and `favorites`.
- Default pixel workflow: `plugins/darkroom/workflow` =
  `scene-referred (sigmoid)` (`data/darktableconfig.xml.in` ~4096). Section
  labels in the QAP are on by default
  (`plugins/darkroom/modulegroups_basics_sections_labels`).

---

## 9. Phase 1: Lightroom-ordered quick access panel (next)

### 9.1 Scope

A built-in preset **"workflow: lightroom"** whose QAP shows, in this order
and regardless of pipeline order:

1. section "basic": `exposure/exposure`, `colorbalancergb/global vibrance`,
   `colorbalancergb/global saturation`
2. section "tone curve": `rgbcurve/channel`, `rgbcurve/curve`

All other presets keep today's behaviour. No new image processing.

### 9.2 Design

1. **Order flag in the preset string.** Use the first reserved field of the
   QAP section: `basics_show|1|` means "list QAP items in stored order".
   Empty or `0` means pipeline order (all existing presets). Old builds and
   upstream ignore the field, so the format stays compatible both ways.
   - add `gboolean basics_listed_order, edit_basics_listed_order;` to
     `dt_lib_modulegroups_t` (struct at line ~118)
   - `_preset_to_string()`: write `"ꬹ%d|%d|"` with the flag
   - `_preset_from_string()`: read `gr2[1]`; reset the flag to FALSE first
     so switching presets cannot leak it
   - new macro next to `SQA`/`SNQA`, e.g. `SLQA()` = start a listed QAP:
     `tx = g_strdup("1|0ꬹ1|1|")`
2. **Section labels.** Allow QAP items whose id starts with `@` (e.g.
   `@basic`, `@tone curve`): a Lightroom-style section heading. Module ops
   never start with `@`, so in pipeline mode and in old builds such items
   simply match nothing and are ignored; the editor list skips them; saving
   keeps them. Render with `dt_ui_section_label_new(C_("modulegroup", name))`
   packed into `d->vbox_basic`, and force the next widget to start a new
   module box (`NEW_MODULE`). Mark the names translatable where the preset
   is defined.
3. **Listed-order builder.** In `_basics_show()`, when the flag is set, call
   a new `_basics_show_listed()` instead of the pipeline loop:
   - iterate `d->basics` in list order
   - for each item, find its module instance the same way pipeline mode
     does: walk `darktable.develop->iop` from the end, first module with
     matching `op`, `!dt_iop_is_hidden()`, not `IOP_FLAGS_DEPRECATED`,
     `iop_order != INT_MAX`
   - on/off items (`WIDGET_TYPE_ACTIVATE_BTN`): `_basics_add_widget(self,
     item, NULL, pos)` as today
   - widget items: find the widget in the module's widget tree whose action
     id equals `item->id` (same action walk as
     `_basics_add_items_from_module_widget()`; factor out a small helper
     that returns the widget for an id rather than copying the loop)
   - `pos` = `FIRST_MODULE` for the first placed item, `NEW_MODULE` when the
     module differs from the previous placed item's module or after a
     section label, else `NORMAL`
   - skip items whose module or widget is missing (do not advance the
     previous-module tracking for them)
   - keep the `DT_ENTER_GUI_UPDATE()`/`DT_LEAVE_GUI_UPDATE()` scope that
     `_basics_show()` already has (see the comment at line ~781)
4. **Editor display** (`_manage_editor_basics_update_list()`): when
   `d->edit_basics_listed_order`, list items in stored order (skip `@`
   items or show them as headings). Optional polish; ordering on save is
   already correct.
5. **Preset definition** in `init_presets()`, after "workflow:
   scene-referred" (line ~1947):
   - `SLQA(); AM("@basic"); AM("exposure/exposure");
     AM("colorbalancergb/global vibrance");
     AM("colorbalancergb/global saturation"); AM("@tone curve");
     AM("rgbcurve/channel"); AM("rgbcurve/curve");`
   - module tabs for Phase 1: one `SMG(C_("modulegroup", "basic"), "basic")`
     with `exposure`, the workflow's tone mapper (`sigmoid`/`filmicrgb`/`agx`
     like the scene-referred preset does with `wf_*`), `temperature`,
     `channelmixerrgb`, `colorbalancergb`, `rgbcurve`, `tonecurve`. Phase 3
     replaces this with the full Lightroom tab set.
   - `dt_lib_presets_add(_("workflow: lightroom"), ...)`
6. Do not change `RECOMMENDED_BASICS` or any existing preset.

### 9.3 Acceptance criteria

- builds without new warnings
- darkroom: module-group presets menu lists "workflow: lightroom";
  selecting it shows exactly the order in 9.1 under the two headings
- switching back to "workflow: scene-referred" shows the old pipeline-ordered
  QAP, unchanged
- moving exposure / vibrance / saturation / the curve enables the module if
  off, adds a history item, and Ctrl+Z undoes it
- duplicate the preset in the preset editor: the copy keeps the order and the
  headings
- a preset string written by an older build loads with pipeline order
- `darktable-cli` output for the regression samples is unchanged (sanity)

### 9.4 GUI verification steps (also closes Phase 0's darkroom check)

1. Quit darktable (Cmd+Q), `cmake --install build`, relaunch via Terminal,
   wait ~40 s.
2. Lighttable, filemanager (grid) layout, select `DSC04299` and `DSC04309`,
   history stack > "discard history", confirm. Their sidecars and library
   entries hold only the 11 auto-applied default modules, recorded while the
   file could not be decoded (rawprepare white point 0, which renders black).
   `DSC04300` is already reset. Nothing the owner edited is lost.
3. Open `DSC04309` in the darkroom: the image must render normally.
4. Select "workflow: lightroom" in the module-group presets menu (the
   hamburger/presets icon at the top of the right panel's module groups) and
   screenshot the QAP; check 9.3.

### 9.5 Result (commit `69abab414`)

Implemented by Kimi on `kimi/phase1-qap-order` (brief
`.local-logs/kimi-task-phase1-impl.md`, report
`.local-logs/kimi-phase1-impl.out.md`), cherry-picked, then one review fix:
Kimi's helper returned only the first action targeting a widget, but
`dt_bauhaus_widget_set_label()` (`bauhaus.c` ~1219) defines an extra action
each time it is called (e.g. tone equalizer's simple-tab sliders), so a QAP id
using a later action would silently stop matching, in pipeline mode too. Now
`_basics_widget_has_id()` compares every action that targets the widget, and
`_basics_add_items_from_module_widget()` is upstream's code except for one
line (`ac = _basics_next_action(ac)`).

Kimi's two justified deviations: headings are translated with
`Q_("modulegroup|<name>")` (glib's `C_` only takes literals; same pattern as
`splittoning.c:481`), and the `NC_` markers are `(void)`-cast to avoid
`-Wunused-value`.

Verified on screen 2026-10-10 (screenshots `/tmp/dt-p1-*.png`):
- presets menu lists "workflow: lightroom"
- QAP: heading "basic" > exposure, global vibrance, global saturation;
  heading "tone curve" > rgb curve graph (the R/G/B `rgbcurve/channel` tabs
  are hidden in rgbcurve's default linked mode, as expected)
- tabs: QAP, active modules, one "basic" tab, presets menu
- setting global saturation to +20% from the QAP enabled color balance rgb
  and added history item 11; Cmd+Z removed it and turned the module off
- switching to "workflow: scene-referred" shows the unchanged
  pipeline-ordered QAP
- `data.db` stores `1|0ꬹ1|1||@basic|...` for the new preset and
  `1|0ꬹ1|||...` (unchanged format) for scene-referred

Not verified on screen: duplicating the preset in the preset editor (covered
by code review: `_preset_to_string(self, TRUE)` writes
`edit_basics_listed_order`). Known cosmetic gap: in compact QAP mode
(`modulegroups_basics_sections_labels` off) headings still render.

### 9.6 Risks

- Widgets inside notebooks that are hidden at the time `_basics_show()` runs
  may not be found; the pipeline-mode code has the same constraint.
- `rgbcurve/channel` only matters in "independent channels" mode; it is
  harmless otherwise.
- A module can appear twice in listed mode if the list returns to it
  non-contiguously; acceptable (each block gets its own header).

---

## 10. Phase 2: complete Lightroom's Basic panel

### 10.1 Scope

Extend the "workflow: lightroom" QAP to Lightroom's Basic panel order:
Temp, Tint, Exposure, Contrast, Highlights, Shadows, Whites, Blacks, Texture,
Clarity, Dehaze, Vibrance, Saturation; then the tone curve. Use existing
darktable controls; add a small helper to an existing module only if no
control is close enough (decide per slider, document the decision here).

### 10.2 Mapping (verified)

Source: Kimi's research (`.local-logs/kimi-lr-mapping.out.md`, every path
cited to the registering source line), spot-checked by the orchestrator
(`colorbalancergb.c` brilliance section ~1891, `sigmoid.c:61-62`,
`agx.c:2074`). Paths are action ids as matched by `_action_id()`
(`modulegroups.c` ~706); they use untranslated names; a middle segment exists
only for widgets created inside `DT_IOP_SECTION_FOR_PARAMS` or a
`dt_action_define_iop` section, never for notebook tab labels.

| Lightroom | QAP path (recommended) | Alternatives / caveats |
|---|---|---|
| Temp | `channelmixerrgb/temperature` (`channelmixerrgb.c:4484`) | on by default in scene-referred; `temperature/temperature` is a dead end there (WB module stays at D65 reference) |
| Tint | `channelmixerrgb/hue` (+ `channelmixerrgb/chroma`) (`channelmixerrgb.c:4490-4500`) | only acts with illuminant = custom; no true tint slider in scene-referred; legacy `temperature/tint` |
| Exposure | `exposure/exposure` (`exposure.c:1258`) | on by default (+0.7 EV auto-applied) |
| Contrast | `sigmoid/contrast` (`sigmoid.c:892`, desc :59) | branch on workflow like `SQA()`: `filmicrgb/contrast`, `agx/curve/contrast` |
| Highlights | `colorbalancergb/brilliance/highlights` (`colorbalancergb.c:1908`) | `toneequal/highlights`; `sigmoid/target white` is a display target, not recovery |
| Shadows | `colorbalancergb/brilliance/shadows` (`colorbalancergb.c:1898`) | `toneequal/shadows`; `shadhi/shadows` (spatial, closest LR feel) |
| Whites | `sigmoid/target white` (`sigmoid.c:968`, desc :61) | moves display mapping, not a clip point; `filmicrgb/white relative exposure` (filmic only) |
| Blacks | `sigmoid/target black` (`sigmoid.c:962`, desc :62) | `exposure/black level correction` (an offset, not a toe) |
| Texture | `diffuse/sharpness` (`diffuse.c:1875`) | `atrous/graph` (contrast equalizer); `bilat/detail` is coarser |
| Clarity | `bilat/detail` (`bilat.c:441`, module alias "clarity") | only `detail`, `midtone range`, `mode` are action-addressable in bilat |
| Dehaze | `hazeremoval/strength` (+ `hazeremoval/distance`) (`hazeremoval.c:272-275`) | |
| Vibrance | `colorbalancergb/global vibrance` (`colorbalancergb.c:1831`) | |
| Saturation | `colorbalancergb/global saturation` (`colorbalancergb.c:1870`) | linear alternative `colorbalancergb/global chroma` |
| Tone curve | `rgbcurve/curve` (+ `rgbcurve/channel`) (`rgbcurve.c:1486`, :1522) | `tonecurve/curve` with `tonecurve/channel` |

Off by default (a QAP edit enables them): `colorbalancergb`, `diffuse`,
`atrous`, `bilat`, `hazeremoval`, `shadhi`, `rgbcurve`, `tonecurve`,
`toneequal`. On by default in the sigmoid workflow: `exposure`,
`channelmixerrgb`, `sigmoid`.

Avoid filmic labels containing non-ASCII characters (e.g. "shadows ↔
highlights balance") in preset strings.

### 10.3 Decisions (orchestrator, 2026-10-10) and delegation

The table above came from Kimi's research. Two of its recommendations were
overruled after checking the code:

- **Texture is not `diffuse/sharpness`.** Every diffusion speed in
  `diffuse.c` defaults to 0 (`first`..`fourth`, `$DEFAULT: 0.`), so moving
  sharpness alone from the QAP would do nothing.
- **Texture / Clarity use `contrastntexture`**
  ("contrast & texture", `src/iop/contrastntexture.c`). The ids are
  `contrastntexture/coarse details` and `/medium details`.
- **Highlights / Shadows were first put on `contrastntexture` too.** On
  2026-10-10 the owner moved them to the tone equalizer bands labeled
  "highlights" (`toneequal.highlights`) and "shadows"
  (`toneequal.deep_blacks`). The quick-access ids are `toneequal/highlights`
  and `toneequal/shadows`. Whites and Blacks stay on the tone mapper.

Final Phase 2 QAP order (brief: `.local-logs/kimi-task-phase2-3-impl.md`):

1. `@white balance`: `channelmixerrgb/illuminant`, `/temperature`, `/hue`,
   `/chroma` (the same controls upstream's scene-referred QAP uses; temperature
   and hue only show for illuminants that use them)
2. `@tone`: `exposure/exposure`; contrast by workflow (`sigmoid/contrast`,
   `filmicrgb/contrast`, `agx/curve/contrast`, else `colorbalancergb/contrast`);
   `toneequal/highlights`, `toneequal/shadows`; whites/blacks by
   workflow (`sigmoid/target white` + `target black`; filmic white/black
   relative exposure; AgX only if Kimi can prove the path from
   `_add_exposure_box()` in `agx.c`)
3. `@presence`: `contrastntexture/medium details` (Texture),
   `contrastntexture/coarse details` (Clarity), `hazeremoval/strength`
   (Dehaze), `colorbalancergb/global vibrance`, `colorbalancergb/global
   saturation`
4. `@tone curve`: `rgbcurve/channel`, `rgbcurve/curve`

Because the order follows Lightroom, a module can appear in two blocks
(e.g. sigmoid for contrast and again for whites/blacks); each block gets its
own module header. Accepted.

Verification to do on screen after Kimi's commit: every listed control is
present and labeled; each one changes the image when moved; enabling and
undo work as in 9.5.

### 10.4 Acceptance

Same checks as 9.3, for every new control; plus a visual comparison on a few
of the owner's photos that each slider moves the image the way the Lightroom
slider of the same name would (direction and rough strength).

### 10.5 Result (commit `cdef3a297`)

Implemented by Kimi (brief `.local-logs/kimi-task-phase2-3-impl.md`, report
`.local-logs/kimi-phase2-3-impl.out.md`, which cites the registering source
line for every id). Kimi also proved the AgX whites/blacks ids: the sliders
are built on `DT_IOP_SECTION_FOR_PARAMS(self, NULL, settings_page)`
(`agx.c` ~2487), a section with no name, so the ids are
`agx/white relative exposure` and `agx/black relative exposure`. The
spektrafilm and "none" workflows get no whites/blacks row; contrast falls
back to `colorbalancergb/contrast` there. The review found nothing to change.

Verified on screen 2026-10-10 on `DSC04309` (sigmoid workflow; screenshots
`/tmp/dt-p2-*.png`):
- headings and controls in exactly the 10.3 order; sigmoid and contrast &
  texture each appear in two blocks with their own header, and the header
  on/off buttons of both blocks stay in sync
- color calibration's illuminant is "custom" on this photo, so temperature
  is hidden and hue/chroma are shown (expected)
- highlights +1.00 and medium details +50% enabled contrast & texture
  (history item 11); target black 0.5% added history item 12 (sigmoid); the
  image visibly changed (RMSE 3.4%: lifted shadows, more stone texture,
  brighter highlights); five Cmd+Z presses restored the 10 default history
  items

Not exercised on screen: the filmic and AgX branches (they need the workflow
preference changed and darktable restarted), and each remaining slider
individually.

---

## 11. Phase 3: Lightroom panels as module tabs

Replace Phase 1's single tab in "workflow: lightroom" with Lightroom-shaped
tabs (module lists use darktable op names; avoid deprecated modules:
`defringe`, `clipping`, `vibrance`, `channelmixer`, `levels`, `basicadj`,
`colisa`, ... see `rg -l IOP_FLAGS_DEPRECATED src/iop`):

- basic (`basic`): `temperature`, `channelmixerrgb`, `exposure`, workflow tone
  mapper, `contrastntexture`, `toneequal`, `hazeremoval`, `colorbalancergb`,
  `highlights`
- tone curve (`tone`): `rgbcurve`, `tonecurve`, `rgblevels`
- color (`color`): `colorequal` (HSL / color mixer), `colorzones`,
  `colorbalancergb` (color grading)
- detail (`correct`): `sharpen`, `diffuse`, `denoiseprofile`, `nlmeans`
- lens & transform (`technical`): `lens`, `cacorrect`, `cacorrectrgb`,
  `hotpixels`, `ashift`, `crop`, `flip` (Lightroom's Lens Corrections and
  Transform panels share one tab because there are only seven distinct
  group icons)
- effects (`effect`): `vignette`, `grain`
- calibration (`grading`): `channelmixerrgb`

That is seven tabs with seven distinct icons (`basic`, `tone`, `color`,
`correct`, `technical`, `effect`, `grading`).

Acceptance: tabs appear in this order with these modules; no deprecated
module listed; other presets unchanged.

### 11.1 Result (commit `9a9f4add5`)

Implemented by Kimi as specified; all 31 module ops exist and none is
deprecated. Verified on screen: the module-group row shows the QAP, active
modules, then seven tabs with seven distinct icons (basic, tone curve,
color, detail, lens & transform, effects, calibration). Contents of each
tab were not opened one by one.

---

## 12. Backlog

The parametric curve (A), Lightroom XMP import (B), before/after on `\` (C)
and Auto tone (D) are no longer unscheduled: they are specified in
`ARCHITECTURE-lightroom-workflow.md` and wait only for owner approval of
that v1 freeze.

Still not scheduled:

- AI masks (subject/sky): needs `-DUSE_AI=ON` and `onnxruntime`.
- Native RawSpeed support for Sony YCbCr ARWs (7.5).
- Startup time (3.3).

---

## 13. Known issues and gotchas (quick list)

- darktable-cli reads sidecars; test with sidecar-free copies (3.4).
- darktable-cli writes `_01`, `_02` suffixes instead of overwriting (3.4).
- Quit the GUI with Cmd+Q before `cmake --install` (3.3).
- Launch the GUI via Terminal, not from an agent's background shell (3.3).
- ~40 s GUI startup is normal on this Mac (3.3).
- `jsonschema` and `libomp` workarounds live in `build/CMakeCache.txt`;
  deleting `build/` means re-running the configure command (3.2).
- Never run `brew upgrade` as part of this work (3.1).
- Pre-commit hook errors under bash 3.2 are harmless (4).
- Peekaboo coordinates are screen points; check the screen is free (3.6).
- In the lighttable culling layout, actions apply to the hovered image (3.6).
- `DSC04299`/`DSC04309` still have stale default histories (9.4 step 2).
- Upstream's own scene-referred QAP entry `AM("agx/auto tune levels")`
  (`modulegroups.c` ~1633) looks stale: the action is registered under the
  "exposure range" section (`agx.c:2074`), so the matching id would be
  `agx/exposure range/auto tune levels`. Pre-existing; not fixed here. Use
  the full path if Phase 2 needs it.

---

## 14. Upstreaming (if the owner ever wants it)

- Phase 0 is a candidate upstream fix on its own (Sony M/S lossless ARWs are
  broken for every darktable user). It would need a body explaining the
  root cause (7.2), a `RELEASE_NOTES.md` entry in its own commit, and AI
  disclosure in the PR description, per `AGENTS.md`.
- The workflow preset (Phases 1-3) is a UX proposal; discuss with upstream
  before opening a PR.
- Exclude this roadmap's commit and `run-dev.sh` from any PR.

---

## 15. Glossary

- **IOP / module**: an image-processing module in `src/iop/` (exposure,
  sigmoid, ...). Each image has an instance of every module; most are off.
- **pixelpipe**: the fixed-order chain of enabled modules that renders an
  image.
- **QAP**: quick access panel, the darkroom panel that shows selected
  controls from many modules.
- **module-group preset**: a named layout of QAP items and module tabs
  (`src/libs/modulegroups.c`).
- **history / sidecar**: the list of edits for an image, stored in the
  library database and in `IMAGE.EXT.xmp` next to the file.
- **sRAW**: darktable's term for raw files whose data is already demosaiced
  3-channel camera RGB (`DT_IMAGE_S_RAW`), e.g. Canon sRAW, and now Sony
  YCbCr ARWs.
- **CFA / Bayer**: a mosaic raw with one color per pixel; needs demosaicing.
