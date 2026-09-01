// Tests for the NDI runtime loader seam (src/NDIRuntimeLoader.h): the
// sibling-DLL path derivation is pure string work exercised on every
// platform; the preload plumbing (module-relative LoadLibrary with a
// system-search fallback) is exercised on Windows only, where it exists.
// Build & run: make test (macOS) / ctest (Windows)

#include "NDIRuntimeLoader.h"

#include <cstdio>
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

static void testSiblingDllPath()
{
    using ndi_loader::siblingDllPath;

    // The real bundle layout (spec decision 16).
    expectTrue(siblingDllPath(
                   L"C:\\Program Files\\Common Files\\OFX\\Plugins\\"
                   L"NDIOutput.ofx.bundle\\Contents\\Win64\\NDIOutput.ofx",
                   L"Processing.NDI.Lib.Advanced.x64.dll") ==
                   L"C:\\Program Files\\Common Files\\OFX\\Plugins\\"
                   L"NDIOutput.ofx.bundle\\Contents\\Win64\\"
                   L"Processing.NDI.Lib.Advanced.x64.dll",
               "bundle layout resolves beside the module");

    // Forward slashes (CMake-staged paths in dev trees).
    expectTrue(siblingDllPath(L"D:/repos/x/stage/Win64/NDIOutput.ofx", L"a.dll") ==
                   L"D:/repos/x/stage/Win64/a.dll",
               "forward-slash separators work");

    // Mixed separators: the LAST separator of either kind ends the directory.
    expectTrue(siblingDllPath(L"C:\\stage/Win64\\NDIOutput.ofx", L"a.dll") ==
                   L"C:\\stage/Win64\\a.dll",
               "mixed separators pick the last one");
    expectTrue(siblingDllPath(L"C:\\stage\\Win64/NDIOutput.ofx", L"a.dll") ==
                   L"C:\\stage\\Win64/a.dll",
               "mixed separators pick the last one (slash last)");

    // No directory component: cannot resolve, empty result (caller falls
    // back to the system search).
    expectTrue(siblingDllPath(L"NDIOutput.ofx", L"a.dll").empty(),
               "bare filename yields empty (no directory to resolve against)");
    expectTrue(siblingDllPath(L"", L"a.dll").empty(),
               "empty module path yields empty");

    // Non-ASCII directory names survive untouched (UTF-16 in, UTF-16 out).
    expectTrue(siblingDllPath(L"C:\\st\u00E9r\u00E9o\\NDIOutput.ofx", L"a.dll") ==
                   L"C:\\st\u00E9r\u00E9o\\a.dll",
               "non-ASCII path preserved");
}

#ifdef _WIN32
static void testPreload()
{
    using ndi_loader::preloadNDIRuntime;

    // A DLL that exists nowhere: both the module-relative attempt and the
    // system fallback must fail, and both error codes must be reported.
    {
        ndi_loader::PreloadResult r =
            preloadNDIRuntime(L"NDIOutput.test.no.such.library.dll");
        expectTrue(!r.loaded, "nonexistent DLL reports not loaded");
        expectTrue(!r.bundlePath.empty(),
                   "module-relative path was derived for the attempt");
        expectTrue(r.bundleError != 0, "bundle attempt error code captured");
        expectTrue(r.systemError != 0, "system attempt error code captured");
    }

    // A DLL that is not beside the module but always on the system search
    // path: the fallback must find it and say so.
    {
        ndi_loader::PreloadResult r = preloadNDIRuntime(L"kernel32.dll");
        expectTrue(r.loaded, "system DLL loads via fallback");
        expectTrue(!r.fromBundle, "system DLL reported as fallback, not bundle");
    }
}
#endif

int main()
{
    testSiblingDllPath();
#ifdef _WIN32
    testPreload();
#endif
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all NDI loader tests passed\n");
    return 0;
}
