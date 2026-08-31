// Module-relative NDI runtime resolution (spec decision 12). The NDI DLL
// ships inside the bundle, beside NDIOutput.ofx — but Windows' loader never
// searches a DLL's own folder when resolving that DLL's imports, so a plain
// import would fail to load the plugin (silently missing from the Effects
// Library) on exactly the machines the bundled DLL exists for. The import is
// therefore delay-loaded (CMakeLists.txt), and before the first NDI call the
// plugin preloads the DLL from its own directory by full path; the delay-load
// helper's later by-name LoadLibrary then resolves to the already-loaded
// module instead of searching disk.
//
// Why preload-by-full-path and not AddDllDirectory (the spec's sketch):
// AddDllDirectory only affects searches that pass LOAD_LIBRARY_SEARCH_USER_DIRS
// — the delay-load helper's plain LoadLibrary does not — unless the process
// calls SetDefaultDllDirectories, which would rewire DLL resolution for ALL of
// Resolve's own loads. A plugin must never mutate its host's process-global
// loader state, so the only self-contained mechanism is loading the DLL
// ourselves before the delay-load helper ever runs.
//
// The path derivation is pure string work, kept platform-neutral so the unit
// suite exercises it everywhere (mirroring PlatformPaths.h).

#ifndef NDI_RUNTIME_LOADER_H
#define NDI_RUNTIME_LOADER_H

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ndi_loader {

// Path of `dllName` in the directory containing `modulePath`. Empty when
// modulePath has no directory component (caller falls back to the system
// search). Both '\' and '/' end a directory; the last of either wins.
inline std::wstring siblingDllPath(const std::wstring& modulePath,
                                   const std::wstring& dllName)
{
    const size_t sep = modulePath.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return std::wstring();
    }
    return modulePath.substr(0, sep + 1) + dllName;
}

#ifdef _WIN32

struct PreloadResult {
    bool loaded = false;
    bool fromBundle = false;        // true: resolved beside the plugin module
    std::wstring bundlePath;        // the module-relative path attempted
    unsigned long bundleError = 0;  // GetLastError of the bundle attempt
    unsigned long systemError = 0;  // GetLastError of the fallback attempt
};

// Load `dllName` into the process: first by full path from the directory of
// the module this code is compiled into (the .ofx), then by name through the
// normal system search (a machine with an NDI runtime install). Idempotent —
// LoadLibrary on an already-loaded module just bumps its refcount, and the
// plugin never unloads the runtime. Never throws, never fails the plugin
// load: on total failure the caller logs both error codes and keeps NDI off.
inline PreloadResult preloadNDIRuntime(const wchar_t* dllName)
{
    PreloadResult result;

    // The module containing this function: the plugin DLL in production, the
    // test executable under the unit suite. UNCHANGED_REFCOUNT because the
    // module cannot vanish while its own code is executing.
    HMODULE selfModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&preloadNDIRuntime),
                           &selfModule)) {
        // GetModuleFileNameW truncates silently (returning the buffer size)
        // when the path doesn't fit; grow until it does.
        std::wstring modulePath(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(
                selfModule, &modulePath[0], static_cast<DWORD>(modulePath.size()));
            if (n == 0) {
                modulePath.clear();
                break;
            }
            if (n < modulePath.size()) {
                modulePath.resize(n);
                break;
            }
            modulePath.resize(modulePath.size() * 2);
        }
        result.bundlePath = siblingDllPath(modulePath, dllName);
    }

    if (!result.bundlePath.empty()) {
        // ALTERED_SEARCH_PATH: the NDI DLL's own dependencies (if it ever
        // grows any) resolve from its directory, not the host executable's.
        if (LoadLibraryExW(result.bundlePath.c_str(), nullptr,
                           LOAD_WITH_ALTERED_SEARCH_PATH)) {
            result.loaded = true;
            result.fromBundle = true;
            return result;
        }
        result.bundleError = GetLastError();
    } else {
        result.bundleError = ERROR_FILE_NOT_FOUND;
    }

    // Fallback: a system-wide NDI runtime installation (search order includes
    // the runtime's PATH entry that installer adds).
    if (LoadLibraryW(dllName)) {
        result.loaded = true;
        return result;
    }
    result.systemError = GetLastError();
    return result;
}

#endif // _WIN32

} // namespace ndi_loader

#endif // NDI_RUNTIME_LOADER_H
