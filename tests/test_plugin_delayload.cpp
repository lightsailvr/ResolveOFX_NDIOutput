// Delay-load smoke test (Windows-only, CI-runnable): proves the built plugin
// binary loads in a process that can resolve imports ONLY from System32 -
// the worst case of Resolve's search order, which never includes the
// plugin's own folder. Two regressions die here:
//   1. the NDI import reverting from delay-load to load-time (the plugin
//      would silently vanish from the Effects Library on any machine
//      without an NDI runtime on the search path), and
//   2. a new dynamic dependency creeping in beside the plugin (the z.dll
//      trap: vcpkg's dynamic zlib staged next to the .ofx resolves in unit
//      tests but never inside Resolve).
// The .ofx is copied ALONE into a temp directory first so nothing in the
// build tree can satisfy an import by accident.
//
// Usage: test_plugin_delayload <path-to-NDIOutput.ofx>  (wired by CMake)

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ofxImageEffect.h"

#include <cstdio>
#include <cstring>
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

static const wchar_t kNdiDllName[] = L"Processing.NDI.Lib.Advanced.x64.dll";

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_plugin_delayload <NDIOutput.ofx>\n");
        return 2;
    }

    // Stage the .ofx alone in a fresh temp directory.
    wchar_t tempRoot[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempRoot) == 0) {
        std::fprintf(stderr, "GetTempPathW failed (%lu)\n", GetLastError());
        return 2;
    }
    std::wstring dir = std::wstring(tempRoot) + L"ndi_delayload_" +
                       std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "CreateDirectoryW failed (%lu)\n", GetLastError());
        return 2;
    }
    const std::wstring ofx = dir + L"\\NDIOutput.ofx";
    if (!CopyFileW(argv[1], ofx.c_str(), FALSE)) {
        std::fprintf(stderr, "CopyFileW %ls -> %ls failed (%lu)\n",
                     argv[1], ofx.c_str(), GetLastError());
        return 2;
    }

    expectTrue(GetModuleHandleW(kNdiDllName) == nullptr,
               "NDI runtime not loaded before the plugin");

    // The load under test: imports resolvable from System32 only.
    HMODULE plugin = LoadLibraryExW(ofx.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!plugin) {
        ++failures;
        std::fprintf(stderr,
                     "FAIL plugin loads with System32-only import search "
                     "(error %lu)\n"
                     "  A load-time import outside System32 exists: either "
                     "the NDI import is no longer delay-loaded, or a new "
                     "dynamic dependency (the z.dll trap) crept in. Resolve "
                     "would silently drop the plugin from the Effects "
                     "Library.\n",
                     GetLastError());
    } else {
        std::printf("ok   plugin loads with System32-only import search\n");

        // Delay-load must also be lazy: merely loading the plugin (what
        // Resolve's scanner does) must not pull the NDI runtime in.
        expectTrue(GetModuleHandleW(kNdiDllName) == nullptr,
                   "loading the plugin does not resolve the NDI import");

        typedef int (*GetNumFn)(void);
        typedef OfxPlugin* (*GetPluginFn)(int);
        GetNumFn getNum = reinterpret_cast<GetNumFn>(
            GetProcAddress(plugin, "OfxGetNumberOfPlugins"));
        GetPluginFn getPlugin = reinterpret_cast<GetPluginFn>(
            GetProcAddress(plugin, "OfxGetPlugin"));
        expectTrue(getNum != nullptr, "OfxGetNumberOfPlugins exported");
        expectTrue(getPlugin != nullptr, "OfxGetPlugin exported");
        if (getNum && getPlugin) {
            expectTrue(getNum() == 1, "plugin count is 1");
            OfxPlugin* p = getPlugin(0);
            expectTrue(p != nullptr, "plugin 0 exists");
            if (p) {
                expectTrue(std::strcmp(p->pluginApi,
                                       kOfxImageEffectPluginApi) == 0,
                           "plugin API is the image-effect API");
                expectTrue(p->pluginIdentifier != nullptr &&
                               p->pluginIdentifier[0] != 0,
                           "plugin identifier present");
            }
            expectTrue(getPlugin(1) == nullptr, "no second plugin");
        }
        FreeLibrary(plugin);
    }

    DeleteFileW(ofx.c_str());
    RemoveDirectoryW(dir.c_str());

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all delay-load smoke tests passed\n");
    return 0;
}

#else
int main() { return 0; }
#endif
