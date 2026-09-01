#ifndef _BRAWImmersiveReader_h_
#define _BRAWImmersiveReader_h_

/*
  BRAW SDK-facing half of the camera-metadata projection (issue #11; Windows
  port issue #26): pulls the embedded Apple Immersive calibration JSON
  (OpticalProjectionData) out of an URSA Cine Immersive .braw. Header-only
  interface so the plugin core never sees the SDK's COM-style headers; the
  implementation (src/BRAWImmersiveReader.cpp) compiles the vendored dispatch
  shim (third_party/braw/, Boost-style license), which resolves the API at
  runtime from the host application's own copy — on macOS
  BlackmagicRawAPI.framework out of the host bundle, on Windows
  BlackmagicRawAPI.dll beside the host exe — inside Resolve that is Resolve's
  shipped copy, so the plugin ships no Blackmagic binaries and, on that
  primary path, cannot skew versions against the host. When the host carries
  no copy (the unit-test binary, or a broken install) it falls back to the
  installed Blackmagic RAW SDK, and fails soft when nothing resolves.

  Metadata-only: opening a clip parses headers — no frame decode, no GPU use,
  instant even on multi-GB media. Call it from the same main-thread parameter
  paths that load STMap EXRs (createInstance / instanceChanged), never render.
*/

#if defined(__APPLE__) || defined(_WIN32)

#include <string>

namespace ndi_brawreader {

// Reads the immersive calibration out of the clip at brawPath. On success
// fills *jsonOut with the OpticalProjectionData blob (feed it to
// ndi_brawmap::parseCalibrationJSON) and, when the caller passes non-null,
// *projectionKindOut (e.g. "fish") and *calibrationTypeOut (e.g. "meiRives").
// Every failure — missing file, non-BRAW bytes, a BRAW without immersive
// calibration, no loadable framework — returns false with a human-readable
// *error and never throws.
bool readImmersiveCalibration(const std::string& brawPath,
                              std::string* jsonOut,
                              std::string* projectionKindOut,
                              std::string* calibrationTypeOut,
                              std::string* error);

} // namespace ndi_brawreader

#endif // __APPLE__ || _WIN32

#endif
