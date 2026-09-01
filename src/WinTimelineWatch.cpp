// Windows implementation of src/TimelineClipWatcher.h (ticket #25) — the
// same one-helper-process-plus-one-reader-thread design as the macOS
// implementation (src/TimelineClipWatcher.cpp), with the POSIX spawn/pipe
// machinery replaced by CreateProcessW and an anonymous pipe
// (src/WinTimelineWatch.h holds the spawn/discovery seam and its tests).
// The helper script and its line protocol are shared verbatim; the pipe's
// read end wraps into a FILE* so the reader loop is line-for-line the same
// fgets loop the macOS side runs.
#ifdef _WIN32

#include "TimelineClipWatcher.h"

#include "NDIRuntimeLoader.h"  // siblingDllPath: fusionscript.dll beside Resolve.exe
#include "PlatformPaths.h"     // wideToUtf8 for log lines
#include "WinTimelineWatch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace ndi_timelinewatch {

namespace {

constexpr int kRespawnBackoffSeconds = 30;

// Same sinks as the plugin's ndiWinLog (NDIOutputPlugin.cpp): DebugView/
// WinDbg via OutputDebugStringA, plus the optional NDI_OUTPUT_LOG_FILE
// append sink so Tier-2 log captures include watcher lines. Duplicated here
// because that sink is static to the plugin translation unit — keep the two
// in sync.
void watchLog(const std::string& msg)
{
    const std::string line = "NDI Plugin: TimelineWatch: " + msg + "\n";
    OutputDebugStringA(line.c_str());
    static std::FILE* sink = []() -> std::FILE* {
        const char* path = std::getenv("NDI_OUTPUT_LOG_FILE");
        return (path && *path) ? ndi_path::fopenUtf8(path, "ab") : nullptr;
    }();
    if (sink) {
        std::fwrite(line.data(), 1, line.size(), sink);
        std::fflush(sink);
    }
}

std::string narrowForLog(const std::wstring& wide)
{
    std::string out;
    if (!ndi_path::detail::wideToUtf8(wide.c_str(), &out)) {
        out.assign("(unrepresentable path)");
    }
    return out;
}

std::mutex gMutex;                 // guards everything below
bool gStarted = false;
std::atomic<bool> gStop{false};
std::thread gThread;
HANDLE gHelperProcess = nullptr;
HANDLE gHelperRead = nullptr;      // read end of the helper's stdout pipe
std::function<void(const std::string&)> gOnChange;
std::string gCurrentPath;
bool gHaveReport = false;
std::string gHealthDetail = "not started";

void setHealthLocked(bool ok, const std::string& detail)
{
    gHaveReport = ok;
    gHealthDetail = detail;
}

// Wait for the helper to finish and close its handle. TerminateProcess as
// the fallback: the helper holds no state worth a graceful exit (its stdout
// is already being discarded), and a wedged child must never wedge the
// reader. Returns the child's exit code when it can be read (the WindowsApps
// python stub's 9009 identified itself only through this), -1 otherwise.
long reapHelperLocked()
{
    if (gHelperProcess == nullptr) {
        return -1;
    }
    if (WaitForSingleObject(gHelperProcess, 2000) == WAIT_TIMEOUT) {
        TerminateProcess(gHelperProcess, 0);
        WaitForSingleObject(gHelperProcess, 2000);
    }
    DWORD code = 0;
    const long exitCode =
        GetExitCodeProcess(gHelperProcess, &code) ? static_cast<long>(code) : -1;
    CloseHandle(gHelperProcess);
    gHelperProcess = nullptr;
    return exitCode;
}

// Spawn the helper with its stdout on a pipe. Returns false with a health
// detail already set (caller holds gMutex).
bool spawnHelperLocked()
{
    using namespace ndi_timelinewatch_win;

    const std::wstring modulePath =
        modulePathContaining(reinterpret_cast<const void*>(&spawnHelperLocked));
    const std::wstring script = bundleResourcePath(modulePath, L"ndi_timeline_watch.py");
    if (script.empty() || GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        setHealthLocked(false, "helper script missing from the plugin bundle's Resources");
        return false;
    }

    const PythonDiscovery python = discoverPython();
    if (python.exePath.empty()) {
        setHealthLocked(false, "no 64-bit Python 3 found (PEP 514 registry or PATH) — "
                               "install Python 3 x64 from python.org or set a "
                               "Manual Path clip");
        return false;
    }

    // fusionscript.dll lives beside Resolve.exe — and this code runs inside
    // Resolve's process, so the host executable's own path finds it even in
    // a non-default install directory. Passed to the helper as argv[1]; an
    // empty/missing path just leaves the script on its documented default.
    const std::wstring exePath = modulePathContaining(nullptr);
    const std::wstring fusionscript =
        exePath.empty() ? std::wstring()
                        : ndi_loader::siblingDllPath(exePath, L"fusionscript.dll");

    const std::wstring cmdLine = quoteArg(python.exePath) + L" " + quoteArg(script) +
                                 L" " + quoteArg(fusionscript);

    HelperProcess proc;
    std::string error;
    if (!spawnHelperProcess(cmdLine, &proc, &error)) {
        setHealthLocked(false, "helper spawn failed: " + error);
        return false;
    }
    gHelperProcess = proc.process;
    gHelperRead = proc.readPipe;
    setHealthLocked(false, "helper starting");  // healthy once the first line lands
    watchLog("helper started (" + narrowForLog(python.exePath) + " [" + python.source +
             "] " + narrowForLog(script) + ")");
    return true;
}

// NOTE handle ownership: only the reader thread ever closes gHelperRead (via
// fclose of the FILE* wrapping it). Other threads must not close it — the
// reader may be blocked in fgets on that handle. Shutdown instead terminates
// the helper; its death closes the pipe's last write handle, which EOFs the
// read side and unblocks the reader (the spawn seam guarantees the read end
// is never inherited by the child, so no other write handle can exist).

// Reader thread: consume helper lines, dedupe, fan out changes; respawn the
// helper (with backoff) whenever it dies, until shutdown.
void readerLoop()
{
    std::string lastAnnouncedError;
    while (!gStop.load(std::memory_order_relaxed)) {
        HANDLE readHandle = nullptr;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (gHelperRead == nullptr && !spawnHelperLocked()) {
                if (gHealthDetail != lastAnnouncedError) {
                    lastAnnouncedError = gHealthDetail;
                    watchLog(gHealthDetail);
                }
            }
            readHandle = gHelperRead;
        }
        if (readHandle == nullptr) {
            for (int i = 0; i < kRespawnBackoffSeconds * 10 && !gStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        std::FILE* stream = ndi_timelinewatch_win::fileStreamForReadHandle(readHandle);
        if (stream == nullptr) {
            std::lock_guard<std::mutex> lock(gMutex);
            CloseHandle(gHelperRead);
            gHelperRead = nullptr;
            if (gHelperProcess != nullptr) {
                TerminateProcess(gHelperProcess, 0);
                reapHelperLocked();
            }
            continue;
        }
        char line[4096];
        while (!gStop.load(std::memory_order_relaxed) &&
               fgets(line, sizeof(line), stream) != nullptr) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (line[0] == '!') {
                // Status message from the helper — log deduped.
                std::lock_guard<std::mutex> lock(gMutex);
                const std::string msg(line + 1);
                if (msg != lastAnnouncedError) {
                    lastAnnouncedError = msg;
                    watchLog(msg);
                }
                continue;
            }
            std::function<void(const std::string&)> onChange;
            std::string changed;
            bool fire = false;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (!gHaveReport || gCurrentPath != line) {
                    gCurrentPath = line;
                    changed = gCurrentPath;
                    onChange = gOnChange;
                    fire = true;
                }
                setHealthLocked(true, "watching");
            }
            if (fire) {
                watchLog(changed.empty() ? std::string("playhead clip: (none)")
                                         : "playhead clip: '" + changed + "'");
                if (onChange) {
                    onChange(changed);
                }
            }
        }
        // EOF (helper died) or shutdown. fclose also closes the handle.
        fclose(stream);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gHelperRead = nullptr;
            const long exitCode = reapHelperLocked();
            if (!gStop.load(std::memory_order_relaxed)) {
                setHealthLocked(false, "helper exited — retrying");
                watchLog("helper exited (code " + std::to_string(exitCode) +
                         ") — retrying in 30 s (Manual Path mode is unaffected)");
            }
        }
        for (int i = 0; i < kRespawnBackoffSeconds * 10 && !gStop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace

void ensureStarted(std::function<void(const std::string&)> onChange)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (gStarted) {
        return;
    }
    gStarted = true;
    gStop.store(false);
    gOnChange = std::move(onChange);
    gThread = std::thread(readerLoop);
}

std::string currentClipPath()
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gCurrentPath;
}

bool healthy(std::string* detail)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (detail) {
        *detail = gHealthDetail;
    }
    return gHaveReport;
}

void shutdown()
{
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gStarted) {
            return;
        }
        gStop.store(true);
        // Killing the helper closes the pipe's only write handle, which EOFs
        // the read side and unblocks the reader's fgets; the reader owns the
        // read handle (see above).
        if (gHelperProcess != nullptr) {
            TerminateProcess(gHelperProcess, 0);
        }
    }
    if (gThread.joinable()) {
        gThread.join();
    }
    std::lock_guard<std::mutex> lock(gMutex);
    reapHelperLocked();  // reader exited before reaping (spawn-window race)
    gStarted = false;
    gOnChange = nullptr;
    setHealthLocked(false, "stopped");
}

} // namespace ndi_timelinewatch

#endif // _WIN32
