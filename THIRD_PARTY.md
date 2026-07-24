# Third-party provenance

Shadow is licensed as `GPL-3.0-or-later`. This file is the intake contract for
code, calibration data, look definitions, and other material obtained from a
third party. It is intentionally separate from ordinary build dependencies:
the latter retain their own package-manager metadata and licenses.

## Current bundled camera-look assets

Shadow bundles three generic maker base curves for Canon, Nikon, and Sony
source rendering. They are not vendor profiles and do not claim camera-model
or in-camera-JPEG equivalence.

## Acceptable sources

Direct integration is allowed only after confirming that the *specific file*
and the planned use are compatible with `GPL-3.0-or-later`. A source project
being public is not sufficient.

For every direct import, add a row below and retain the original license and
copyright notice beside the imported material.

| Component | Upstream project and immutable revision | Original path | License | Shadow path | Modification note |
| --- | --- | --- | --- | --- | --- |
| Generic Canon/Nikon/Sony base-curve samples | darktable `b0bd5b40816b0e5904fa264542e738760eb0074d` | `src/iop/basecurve.c` | GPL-3.0-or-later | `cpp/shadow-image/src/color/source_profile_catalog.cpp` | Copied only the three generic six-point curve data sets; Shadow evaluates them using its own luminance-preserving monotone interpolator. |

The expected first candidates are carefully selected, file-level-audited data
or algorithms from darktable and RawTherapee. A prior study of either project,
or a clean-room implementation informed by published behavior, is not a direct
import and must not be presented as one.

## Behavioral references (no imported code or bundled data)

- Shadow's bounded DCP parser, exact-model catalog, dual-illuminant selection,
  ForwardMatrix preference and ColorMatrix fallback were independently
  implemented from the public Adobe DNG tag/colour-transform definitions.
  RawTherapee revision `039b9b89d43315be6b42e8fbb33b8cfb39edd4bf`,
  `rtengine/dcp.cc` and `rtengine/dcp.h` (GPL-3.0-or-later), was inspected as a
  mature interoperability reference. No RawTherapee source was copied into
  Shadow.
- A developer test may read a RawTherapee checkout's
  `rtdata/dcpprofiles/NIKON Z 9.dcp` through
  `SHADOW_TEST_RAWTHERAPEE_DCP_PROFILE`. The profile remains outside this
  repository and is neither installed, cached, nor redistributed by Shadow.

## Camera profiles and proprietary material

- Do not copy, bundle, or parse profiles from Adobe or camera-vendor software
  unless the individual file has an explicit redistribution permission that
  has been recorded here.
- A user-installed private provider is not part of the public Shadow source or
  binary distribution. Shadow exchanges only documented provider-protocol
  values with it; vendor SDK headers, binaries, resources, and decoded-profile
  logic stay outside this repository.
- User-local profiles may be selected by the application only through a
  declarative, documented Shadow profile format. Shadow must not silently copy
  an opaque vendor file into its catalog, cache, exports, or bug reports.

## Required attribution on adaptation

When adapting a third-party code or data file, keep its original notice and
add a short header containing: upstream URL, immutable revision, original
path, date of import, and a factual summary of Shadow's changes. Keep the
original license text with the imported material. Do not rewrite provenance
into a generic project-level credit.
