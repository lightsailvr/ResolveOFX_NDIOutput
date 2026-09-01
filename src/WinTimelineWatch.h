// Support seam for the Windows timeline-clip watcher (ticket #25): the pure
// pieces of "find a Python, build a command line, find the bundled helper
// script" live here so the unit suite exercises them on every platform
// (mirroring NDIRuntimeLoader.h / PlatformPaths.h), and the Windows-only
// plumbing (PEP 514 registry discovery, CreateProcessW with a stdout pipe)
// is thin enough to smoke-test with cmd.exe in tests/test_timeline_watch.cpp.
// The watcher itself — reader thread, respawn/backoff, the line protocol —
// is src/WinTimelineWatch.cpp, implementing the TimelineClipWatcher.h API.

#ifndef WIN_TIMELINE_WATCH_H
#define WIN_TIMELINE_WATCH_H

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>

namespace ndi_timelinewatch_win {

// ---- Python discovery (PEP 514) -------------------------------------------
// Resolve's Windows scripting DLL (fusionscript.dll) is x64 and loads as a
// CPython extension, so the helper needs a 64-bit Python 3. The registry is
// where python.org installers record themselves (PEP 514): tags under
// Software\Python\PythonCore are "X.Y", with "-32"/"-arm64" suffixes marking
// interpreters that could never load the x64 DLL.

struct PyVersion {
    int major = 0;
    int minor = 0;
};

// Parse a PythonCore tag. Accepts exactly "X.Y" with X >= 3; rejects
// suffixed (non-x64) tags and anything else.
inline bool parsePythonTag(const std::wstring& tag, PyVersion* out)
{
    const size_t dot = tag.find(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= tag.size()) {
        return false;
    }
    int major = 0, minor = 0;
    for (size_t i = 0; i < dot; ++i) {
        if (tag[i] < L'0' || tag[i] > L'9') {
            return false;
        }
        major = major * 10 + (tag[i] - L'0');
    }
    for (size_t i = dot + 1; i < tag.size(); ++i) {
        if (tag[i] < L'0' || tag[i] > L'9') {
            return false;  // "-32", "-arm64", "3.11.4", garbage
        }
        minor = minor * 10 + (tag[i] - L'0');
    }
    if (major < 3) {
        return false;
    }
    out->major = major;
    out->minor = minor;
    return true;
}

struct PythonCandidate {
    std::wstring tag;      // PythonCore subkey name, e.g. L"3.11"
    std::wstring exePath;  // full path to python.exe
    std::string source;    // where it was found, for the log
};

// Index of the highest-versioned usable candidate; candidates.size() when
// none parse. Strictly-greater comparison, so on a version tie the
// first-seen candidate wins — callers feed HKCU before HKLM, which is
// PEP 514's precedence order.
inline size_t pickBestPythonIndex(const std::vector<PythonCandidate>& candidates)
{
    size_t best = candidates.size();
    PyVersion bestV;
    for (size_t i = 0; i < candidates.size(); ++i) {
        PyVersion v;
        if (!parsePythonTag(candidates[i].tag, &v)) {
            continue;
        }
        if (best == candidates.size() || v.major > bestV.major ||
            (v.major == bestV.major && v.minor > bestV.minor)) {
            best = i;
            bestV = v;
        }
    }
    return best;
}

inline std::wstring pickBestPython(const std::vector<PythonCandidate>& candidates)
{
    const size_t i = pickBestPythonIndex(candidates);
    return i < candidates.size() ? candidates[i].exePath : std::wstring();
}

// ---- Command-line quoting --------------------------------------------------
// One argument, quoted so CommandLineToArgvW / the MSVC CRT parse it back
// verbatim: quotes only when needed (readable logs), backslash runs before a
// quote double, embedded quotes escape.
inline std::wstring quoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out(1, L'"');
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(c);
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// ---- Bundle-resource path ---------------------------------------------------
// From the plugin module (…\Contents\Win64\NDIOutput.ofx) to a file in the
// bundle's Resources (…\Contents\Resources\<name>) — the Windows analogue of
// the macOS watcher's Contents/macOS → Contents/Resources hop. Empty when
// the path is too shallow to go up a level (caller soft-fails with a log).
inline std::wstring bundleResourcePath(const std::wstring& modulePath,
                                       const std::wstring& name)
{
    const size_t last = modulePath.find_last_of(L"\\/");
    if (last == std::wstring::npos || last == 0) {
        return std::wstring();
    }
    const size_t parent = modulePath.find_last_of(L"\\/", last - 1);
    if (parent == std::wstring::npos) {
        return std::wstring();
    }
    return modulePath.substr(0, parent + 1) + L"Resources" + modulePath[last] + name;
}

#ifdef _WIN32

// Full path of the module containing `addr` (nullptr: the host executable —
// Resolve.exe, whose directory holds fusionscript.dll). Empty on failure.
// Same grow-until-it-fits loop as NDIRuntimeLoader.h.
inline std::wstring modulePathContaining(const void* addr)
{
    HMODULE module = nullptr;
    if (addr != nullptr &&
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &module)) {
        return std::wstring();
    }
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(module, &path[0], static_cast<DWORD>(path.size()));
        if (n == 0) {
            return std::wstring();
        }
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// Registry sweep of one root's Software\Python\PythonCore (64-bit view: a
// 32-bit HKLM install lands under WOW6432Node and stays invisible, which is
// correct — it could not load fusionscript.dll anyway; 32-bit HKCU installs
// are filtered by their "-32" tag). Appends only interpreters that exist on
// disk.
inline void collectRegistryPythons(HKEY root, const char* rootName,
                                   std::vector<PythonCandidate>* out)
{
    HKEY core = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Python\\PythonCore", 0,
                      KEY_READ | KEY_WOW64_64KEY, &core) != ERROR_SUCCESS) {
        return;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t tag[256];
        DWORD tagLen = 256;
        const LSTATUS rc =
            RegEnumKeyExW(core, index, tag, &tagLen, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            break;
        }
        const std::wstring installKey = std::wstring(tag) + L"\\InstallPath";
        wchar_t buf[1024];
        DWORD cb = sizeof(buf);
        std::wstring exe;
        if (RegGetValueW(core, installKey.c_str(), L"ExecutablePath",
                         RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf,
                         &cb) == ERROR_SUCCESS) {
            exe = buf;
        } else {
            cb = sizeof(buf);
            if (RegGetValueW(core, installKey.c_str(), nullptr,
                             RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf,
                             &cb) == ERROR_SUCCESS) {
                exe = buf;
                if (!exe.empty() && exe.back() != L'\\' && exe.back() != L'/') {
                    exe += L'\\';
                }
                exe += L"python.exe";
            }
        }
        if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        std::string source = std::string(rootName) + " PythonCore\\";
        for (const wchar_t c : std::wstring(tag)) {
            source += (c < 128) ? static_cast<char>(c) : '?';
        }
        out->push_back({tag, exe, source});
    }
    RegCloseKey(core);
}

struct PythonDiscovery {
    std::wstring exePath;  // empty: no usable interpreter on this machine
    std::string source;    // human-readable provenance for the log
};

// The documented discovery order (BUILD.md, Windows section): PEP 514
// registry — HKCU then HKLM, 64-bit Python 3 only, highest version — then a
// PATH search as the fallback for interpreters that never registered.
inline PythonDiscovery discoverPython()
{
    std::vector<PythonCandidate> candidates;
    collectRegistryPythons(HKEY_CURRENT_USER, "HKCU", &candidates);
    collectRegistryPythons(HKEY_LOCAL_MACHINE, "HKLM", &candidates);
    const size_t best = pickBestPythonIndex(candidates);
    if (best < candidates.size()) {
        return {candidates[best].exePath, candidates[best].source};
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr) > 0 &&
        GetFileAttributesW(found) != INVALID_FILE_ATTRIBUTES) {
        return {found, "PATH search"};
    }
    return {};
}

// ---- Process spawn ----------------------------------------------------------

struct HelperProcess {
    HANDLE process = nullptr;   // caller closes (after the child is gone)
    HANDLE readPipe = nullptr;  // caller owns; fileStreamForReadHandle absorbs it
};

// CreateProcessW with the child's stdout on a fresh anonymous pipe and
// stdin/stderr on NUL. The child is a hidden console process
// (CREATE_NO_WINDOW — Resolve is a GUI app; a console flash per respawn
// would be user-visible), and inherits EXACTLY the three standard handles
// via PROC_THREAD_ATTRIBUTE_HANDLE_LIST: spawning happens inside Resolve's
// process, where unrelated inheritable handles abound, and any of them
// leaking into the child could keep files or sockets alive past their
// owners. Critically the pipe's READ end is never inherited, so the child's
// death is guaranteed to EOF the pipe — the watcher's only wake-up signal.
inline bool spawnHelperProcess(const std::wstring& cmdLine, HelperProcess* out,
                               std::string* error)
{
    auto fail = [&](const char* what) {
        if (error) {
            *error = std::string(what) + " (Win32 error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    };

    SECURITY_ATTRIBUTES inheritable = {};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        return fail("CreatePipe failed");
    }
    // Only the write end may cross into the child.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return fail("opening NUL failed");
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    HANDLE inheritList[2] = {writeEnd, nul};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inheritList, sizeof(inheritList), nullptr,
                                   nullptr)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        CloseHandle(nul);
        return fail("building the handle-inheritance list failed");
    }

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = writeEnd;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attrs;

    // CreateProcessW may scribble on the command-line buffer.
    std::wstring mutableCmd = cmdLine;
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(
        nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &si.StartupInfo, &pi);
    const DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(writeEnd);  // the child holds its own copy now (or never will)
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(readEnd);
        SetLastError(createError);
        return fail("CreateProcessW failed");
    }
    CloseHandle(pi.hThread);
    out->process = pi.hProcess;
    out->readPipe = readEnd;
    return true;
}

// Wrap the pipe's read HANDLE into a FILE* so the reader loop can fgets
// line-by-line exactly like the macOS watcher. Ownership transfers: fclose
// closes the underlying handle. On failure the caller still owns the handle.
inline std::FILE* fileStreamForReadHandle(HANDLE readPipe)
{
    const int fd =
        _open_osfhandle(reinterpret_cast<intptr_t>(readPipe), _O_RDONLY | _O_BINARY);
    if (fd == -1) {
        return nullptr;
    }
    std::FILE* stream = _fdopen(fd, "rb");
    if (stream == nullptr) {
        _close(fd);  // also closes the handle
        return nullptr;
    }
    return stream;
}

#endif // _WIN32

} // namespace ndi_timelinewatch_win

#endif // WIN_TIMELINE_WATCH_H
