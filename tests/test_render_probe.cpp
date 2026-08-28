// Tests for the render-probe log-line formatter (src/RenderProbe.h).
// Build & run: make test

#include "RenderProbe.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void expectEq(const std::string& actual, const std::string& expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n",
                     name, expected.c_str(), actual.c_str());
    } else {
        std::printf("ok   %s\n", name);
    }
}

int main()
{
    using ndi_probe::ProbeRenderInfo;

    {
        // Everything the host can provide, provided.
        ProbeRenderInfo info;
        info.callIndex = 12;
        info.page = "Edit";
        info.hasEye = true; info.eye = 1;
        info.time = 86400.0;
        info.hasSrcFrame = true; info.srcFrame = 86399;
        info.width = 1920; info.height = 1080;
        info.hasScale = true; info.scaleX = 0.5; info.scaleY = 0.5;
        info.hasThumbnail = true; info.thumbnail = 0;
        info.hasDt = true; info.dtMs = 16.68;
        expectEq(ndi_probe::formatProbeLine(info),
                 "probe #000012 page=Edit eye=R time=86400 src=86399 dim=1920x1080 scale=0.50x0.50 thumb=0 dt=16.7ms",
                 "full line");
    }
    {
        // Host provided none of the optional properties (first call, no page string).
        // Every absent property must read as absent, never as a fake value.
        ProbeRenderInfo info;
        info.callIndex = 1;
        info.page = "";
        info.time = 100.0;
        info.width = 3840; info.height = 2160;
        expectEq(ndi_probe::formatProbeLine(info),
                 "probe #000001 page=? eye=- time=100 src=- dim=3840x2160 scale=- thumb=- dt=-",
                 "all optional fields absent");
    }
    {
        // Left eye, thumbnail render, fractional (retimed) frame time.
        ProbeRenderInfo info;
        info.callIndex = 2;
        info.page = "Color";
        info.hasEye = true; info.eye = 0;
        info.time = 100.5;
        info.width = 192; info.height = 108;
        info.hasThumbnail = true; info.thumbnail = 1;
        expectEq(ndi_probe::formatProbeLine(info),
                 "probe #000002 page=Color eye=L time=100.500 src=- dim=192x108 scale=- thumb=1 dt=-",
                 "left eye + thumbnail + fractional time");
    }
    {
        // An eye value the header doesn't define is logged raw, not mislabeled.
        ProbeRenderInfo info;
        info.callIndex = 3;
        info.page = "Fusion";
        info.hasEye = true; info.eye = 2;
        info.time = 0.0;
        info.width = 1; info.height = 1;
        expectEq(ndi_probe::formatProbeLine(info),
                 "probe #000003 page=Fusion eye=2 time=0 src=- dim=1x1 scale=- thumb=- dt=-",
                 "unexpected eye value logged raw");
    }
    {
        // A null page pointer must not crash and reads as unknown.
        ProbeRenderInfo info;
        info.callIndex = 4;
        info.page = nullptr;
        info.time = 1.0;
        info.width = 10; info.height = 10;
        expectEq(ndi_probe::formatProbeLine(info),
                 "probe #000004 page=? eye=- time=1 src=- dim=10x10 scale=- thumb=- dt=-",
                 "null page pointer");
    }

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All render-probe tests passed\n");
    return 0;
}
