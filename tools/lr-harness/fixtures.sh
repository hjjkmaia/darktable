#!/bin/sh
# Copy Lightroom verification fixtures to /tmp/lr-fixtures/.
# Reads the originals. Never writes next to them.
set -eu

dest="/tmp/lr-fixtures"
sony="${HOME}/Desktop/sony/11760513"
flavor="${HOME}/photo-flavor/samples"
identity_src="${sony}/DSC04299.xmp"
moving_src="${flavor}/cinematic-teal-orange.xmp"

mkdir -p "${dest}"

copy() {
  src=$1
  name=$2
  if [ ! -f "${src}" ]; then
    echo "fixtures.sh: missing ${src}" >&2
    exit 1
  fi
  cp -p "${src}" "${dest}/${name}"
}

for base in DSC04299 DSC04300 DSC04302; do
  copy "${sony}/${base}.ARW" "${base}.ARW"
  copy "${sony}/${base}.xmp" "${base}.xmp"
done

# Photo-flavor sample whose contrast, texture, dehaze, and grade luminance
# are all away from zero. XMP only: pair it with a raw before `history`.
copy "${moving_src}" "cinematic-teal-orange.xmp"

# Explicit identity-parametric sidecar (region sliders 0, splits 25/50/75).
# The three DSC*.xmp files above are identity too; this name is the regression copy.
copy "${identity_src}" "parametric-identity.xmp"

moving="${dest}/cinematic-teal-orange.xmp"
if ! grep -Eq 'crs:(Contrast2012|Texture|Dehaze|ColorGrade(Shadow|Midtone|Highlight|Global)Lum)="-?[1-9]' \
    "${moving}"; then
  echo "fixtures.sh: ${moving} does not move contrast, texture, dehaze, or grade luminance" >&2
  exit 1
fi

ident="${dest}/parametric-identity.xmp"
for tag in \
  'crs:ParametricShadows="0"' \
  'crs:ParametricDarks="0"' \
  'crs:ParametricLights="0"' \
  'crs:ParametricHighlights="0"' \
  'crs:ParametricShadowSplit="25"' \
  'crs:ParametricMidtoneSplit="50"' \
  'crs:ParametricHighlightSplit="75"'
do
  if ! grep -Fq "${tag}" "${ident}"; then
    echo "fixtures.sh: parametric-identity.xmp is missing ${tag}" >&2
    exit 1
  fi
done

echo "fixtures in ${dest}:"
ls -1 "${dest}"
