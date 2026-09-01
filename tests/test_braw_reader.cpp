// Tests for the Windows BRAW immersive-metadata reader
// (src/BRAWImmersiveReader.cpp, ticket #26): the soft-fail contract that the
// plugin's passthrough + Stream Status behavior depends on. Every failure —
// no loadable BlackmagicRawAPI.dll, a missing file, non-BRAW bytes — must
// return false with a human-readable error and never throw or crash. The
// tests assert exactly that contract, so they pass both on hosted CI (no
// BRAW runtime anywhere) and on a workstation where the dispatch shim finds
// the installed SDK or Resolve's own DLL.
//
// Full-chain success (real clip -> calibration JSON -> lens maps) can't run
// hosted: it needs the runtime AND an URSA Cine Immersive clip. Set
// NDI_TEST_BRAW_CLIP to such a clip's path to exercise it (the workstation
// half of Tier 0, like test_cuda_downscale's GPU half); unset, that section
// skips cleanly. Build & run: ctest (Windows only — macOS covers the same
// contract through its own reader in the Makefile suite).

#include "BRAWImmersiveReader.h"
#include "BRAWLensMap.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

static void expectTrue(bool actual, const char* name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// Missing file: whether or not a BRAW runtime loads, the reader must say no
// softly. jsonOut is written on success only — a failure must not disturb the
// caller's buffer (same contract as the macOS half).
static void testNonexistentPath()
{
    std::string json = "sentinel", error;
    const bool ok = ndi_brawreader::readImmersiveCalibration(
        "Z:\\definitely\\not\\here\\missing_clip.braw", &json, nullptr, nullptr, &error);
    expectTrue(!ok, "nonexistent path returns false");
    expectTrue(!error.empty(), "nonexistent path yields an error message");
    expectTrue(json == "sentinel", "failure leaves jsonOut untouched");
    std::printf("     (error: %s)\n", error.c_str());
}

// Non-BRAW bytes with a .braw name: OpenClip must reject it (runtime
// present) or the factory must be reported missing (hosted CI) — either way
// false + error, no crash.
static void testGarbageFile()
{
    char tempDir[MAX_PATH] = {0};
    if (GetTempPathA(sizeof(tempDir), tempDir) == 0) {
        expectTrue(false, "GetTempPathA produced a scratch directory");
        return;
    }
    // Deterministic name: a leftover from a killed run just gets overwritten.
    std::string braw = std::string(tempDir) + "ndi_test_braw_reader_garbage.braw";
    FILE* f = nullptr;
    if (fopen_s(&f, braw.c_str(), "wb") != 0 || !f) {
        expectTrue(false, "scratch .braw is writable");
        return;
    }
    static const char junk[] = "this is not a blackmagic raw clip, not even close";
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    std::string json, error;
    const bool ok = ndi_brawreader::readImmersiveCalibration(braw, &json, nullptr, nullptr, &error);
    expectTrue(!ok, "garbage .braw returns false");
    expectTrue(!error.empty(), "garbage .braw yields an error message");
    std::printf("     (error: %s)\n", error.c_str());
    std::remove(braw.c_str());
}

// A path that is not valid UTF-8 must be refused before it reaches the SDK.
static void testInvalidUtf8Path()
{
    std::string bogus = "C:\\clips\\bad_\xFF\xFE_name.braw";
    std::string json, error;
    const bool ok = ndi_brawreader::readImmersiveCalibration(bogus, &json, nullptr, nullptr, &error);
    expectTrue(!ok, "invalid UTF-8 path returns false");
    expectTrue(!error.empty(), "invalid UTF-8 path yields an error message");
}

// Opt-in full chain on a real clip: metadata -> JSON parse -> map generation,
// the exact sequence brawAcquireLensPair runs inside the plugin.
static void testRealClipIfProvided()
{
    char* clip = nullptr;
    size_t len = 0;
    if (_dupenv_s(&clip, &len, "NDI_TEST_BRAW_CLIP") != 0 || clip == nullptr) {
        std::printf("skip real-clip chain (set NDI_TEST_BRAW_CLIP to an URSA Cine "
                    "Immersive .braw to run it)\n");
        return;
    }
    std::string path(clip);
    free(clip);

    std::string json, kind, calType, error;
    const bool read = ndi_brawreader::readImmersiveCalibration(path, &json, &kind, &calType, &error);
    expectTrue(read, "real clip: metadata reads");
    if (!read) {
        std::fprintf(stderr, "     (error: %s)\n", error.c_str());
        return;
    }
    std::printf("     (projection %s, calibration %s, %zu bytes JSON)\n",
                kind.c_str(), calType.c_str(), json.size());

    // Optional parity artifact: dump the raw blob for diffing against the
    // macOS-extracted fixture (tests/fixtures/ursa_immersive_calibration.json
    // came from this same camera).
    char* dump = nullptr;
    if (_dupenv_s(&dump, &len, "NDI_TEST_BRAW_DUMP") == 0 && dump != nullptr) {
        FILE* out = nullptr;
        if (fopen_s(&out, dump, "wb") == 0 && out) {
            fwrite(json.data(), 1, json.size(), out);
            fclose(out);
            std::printf("     (raw blob dumped to %s)\n", dump);
        }
        free(dump);
    }
    expectTrue(kind == "fish", "real clip: projection kind is fish");
    expectTrue(calType == "meiRives", "real clip: calibration type is meiRives");

    ndi_brawmap::LensCalibration cal;
    expectTrue(ndi_brawmap::parseCalibrationJSON(json, &cal, &error),
               "real clip: calibration JSON parses");
    ndi_stmap::STMapImage left, right;
    expectTrue(ndi_brawmap::generateLensMaps(cal, 256, false, &left, &right, &error),
               "real clip: lens maps generate");
    expectTrue(left.width == 256 && left.height == 256 &&
               right.width == 256 && right.height == 256,
               "real clip: maps are the requested size");
}

int main()
{
    testNonexistentPath();
    testGarbageFile();
    testInvalidUtf8Path();
    testRealClipIfProvided();

    if (failures == 0) {
        std::printf("all braw-reader tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d braw-reader test(s) FAILED\n", failures);
    return 1;
}
