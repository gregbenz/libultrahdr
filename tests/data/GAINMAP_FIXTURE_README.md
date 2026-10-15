# Generated routing-probe fixture: `gainmap_hevc_16x16.heic`

This synthetic fixture was generated during test development with this repository's public HEIF
encoder from programmatically initialized, uniform 16x16 P010 HLG pixels. No third-party source
image was used. The encoder used its default gain-map scale, quality, and metadata settings. The
result contains direct-coded HEVC primary and gain-map items plus ISO 21496-1 metadata and exists to
test both sides of runtime decoder availability. It was generated from libultrahdr revision
`418b6b361e252a91c435a56cf386afb37d7d1c9d`; the temporary generation helper was not retained.

SHA-256: `6dd810ee6d118d3ce876936d9b265338b94d44cde187bce2ba384ce93edb1b8e`

## Fixture-specific license

The license below applies specifically to `gainmap_hevc_16x16.heic` and this provenance document;
it is an explicit exception to the general CC-BY-4.0 license used by the pre-existing files in this
directory.

SPDX-License-Identifier: Apache-2.0 OR MIT

## Ordinary AVIF rejection fixture: `ordinary_synthetic.avif`

This is a 16x16 lossless AVIF generated from a deterministic RGB pattern. It contains no external
image data, EXIF, XMP, ICC profile, or gain map. It exercises the decoder-export API's non-JPEG
input rejection path.

Reproduction uses ImageMagick 7.1.2-27 and `avifenc` 1.3.0 with AOM 3.14.1. The standard-library
Python snippet creates the pattern; ImageMagick converts it to PNG; `avifenc` writes the AVIF:

```sh
python3 - <<'PY'
from pathlib import Path
w = h = 16
pixels = bytearray()
for y in range(h):
    for x in range(w):
        pixels.extend(((x * 17) & 255, (y * 17) & 255, ((x ^ y) * 17) & 255))
Path('/tmp/ordinary_synthetic.ppm').write_bytes(
    f'P6\n{w} {h}\n255\n'.encode('ascii') + pixels)
PY
magick /tmp/ordinary_synthetic.ppm -depth 8 /tmp/ordinary_synthetic.png
avifenc --qcolor 100 --depth 8 --yuv 444 --cicp 1/13/0 --speed 10 --jobs 1 \
  /tmp/ordinary_synthetic.png tests/data/ordinary_synthetic.avif
```

SHA-256: `0bb8247849a2056723b2dd7895babf3d325c910ee035a2dc6ce1ead3fa26614b`
