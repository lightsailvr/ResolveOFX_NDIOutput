#ifndef _RenderProbe_h_
#define _RenderProbe_h_

/*
  Render-call diagnostic probe: formatting for the one-line-per-render log.

  Kept free of OFX and NDI dependencies so it is unit-testable without a host
  (tests/test_render_probe.cpp). The plugin gathers the host properties inside
  the render action and calls formatProbeLine().
*/

#include <cstdio>
#include <string>

namespace ndi_probe {

// One render call as fed by the host. `has*` is false when the host did not
// supply the property — the log line must distinguish "absent" from every
// real value, since absence itself is a finding.
struct ProbeRenderInfo {
    long long callIndex = 0;      // 1-based per-instance sequence number
    const char* page = "";        // Resolve page that instantiated the effect; ""/null = host didn't say
    bool hasEye = false;
    int eye = 0;                  // OfxImageEye: 0 = mono/left, 1 = right
    double time = 0.0;            // kOfxPropTime (timeline frame)
    bool hasSrcFrame = false;
    long long srcFrame = 0;       // kOfxImageEffectPropSrcFrame
    int width = 0;                // render window
    int height = 0;
    bool hasScale = false;
    double scaleX = 1.0;          // kOfxImageEffectPropRenderScale
    double scaleY = 1.0;
    bool hasThumbnail = false;
    int thumbnail = 0;            // kOfxImageClipPropThumbnail on the source clip
    bool hasDt = false;
    double dtMs = 0.0;            // wall-clock spacing to the previous render call
};

inline std::string formatProbeLine(const ProbeRenderInfo& info)
{
    char eyeBuf[16];
    if (!info.hasEye) {
        std::snprintf(eyeBuf, sizeof(eyeBuf), "-");
    } else if (info.eye == 0) {
        std::snprintf(eyeBuf, sizeof(eyeBuf), "L");
    } else if (info.eye == 1) {
        std::snprintf(eyeBuf, sizeof(eyeBuf), "R");
    } else {
        std::snprintf(eyeBuf, sizeof(eyeBuf), "%d", info.eye);
    }

    char timeBuf[32];
    if (info.time == static_cast<double>(static_cast<long long>(info.time))) {
        std::snprintf(timeBuf, sizeof(timeBuf), "%lld", static_cast<long long>(info.time));
    } else {
        std::snprintf(timeBuf, sizeof(timeBuf), "%.3f", info.time);
    }

    char srcBuf[32];
    if (info.hasSrcFrame) {
        std::snprintf(srcBuf, sizeof(srcBuf), "%lld", info.srcFrame);
    } else {
        std::snprintf(srcBuf, sizeof(srcBuf), "-");
    }

    char scaleBuf[32];
    if (info.hasScale) {
        std::snprintf(scaleBuf, sizeof(scaleBuf), "%.2fx%.2f", info.scaleX, info.scaleY);
    } else {
        std::snprintf(scaleBuf, sizeof(scaleBuf), "-");
    }

    char thumbBuf[16];
    if (info.hasThumbnail) {
        std::snprintf(thumbBuf, sizeof(thumbBuf), "%d", info.thumbnail);
    } else {
        std::snprintf(thumbBuf, sizeof(thumbBuf), "-");
    }

    char dtBuf[32];
    if (info.hasDt) {
        std::snprintf(dtBuf, sizeof(dtBuf), "%.1fms", info.dtMs);
    } else {
        std::snprintf(dtBuf, sizeof(dtBuf), "-");
    }

    char line[256];
    std::snprintf(line, sizeof(line),
                  "probe #%06lld page=%s eye=%s time=%s src=%s dim=%dx%d scale=%s thumb=%s dt=%s",
                  info.callIndex,
                  (info.page && info.page[0]) ? info.page : "?",
                  eyeBuf, timeBuf, srcBuf,
                  info.width, info.height,
                  scaleBuf, thumbBuf, dtBuf);
    return std::string(line);
}

} // namespace ndi_probe

#endif
