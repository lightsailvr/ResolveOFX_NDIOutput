// Implementation of src/TimelineClipWatcher.h — see that header for the
// design. One helper process + one reader thread per plugin load.
#ifdef __APPLE__

#include "TimelineClipWatcher.h"

#include "MacTimelineWatch.h"  // fuscript/python discovery + spawn seam (tested)

#include <dlfcn.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <os/log.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define WATCH_LOG(str) os_log(OS_LOG_DEFAULT, "NDI Plugin: TimelineWatch: %{public}s", str)

namespace ndi_timelinewatch {

namespace {

constexpr int kRespawnBackoffSeconds = 30;

std::mutex gMutex;                 // guards everything below
bool gStarted = false;
std::atomic<bool> gStop{false};
std::thread gThread;
pid_t gHelperPid = -1;
int gHelperFd = -1;                // read end of the helper's stdout pipe
std::function<void(const std::string&)> gOnChange;
std::string gCurrentPath;
bool gHaveReport = false;
std::string gHealthDetail = "not started";

// Resolve a path relative to this binary (the .ofx inside the bundle):
// Contents/macOS/NDIOutput.ofx -> Contents/Resources/<name>.
std::string bundleResourcePath(const char* name)
{
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&bundleResourcePath), &info) == 0 ||
        info.dli_fname == nullptr) {
        return std::string();
    }
    char pathBuf[PATH_MAX];
    strlcpy(pathBuf, info.dli_fname, sizeof(pathBuf));
    std::string dir = dirname(pathBuf);              // .../Contents/macOS
    std::string candidate = dir + "/../Resources/" + name;
    char resolved[PATH_MAX];
    if (realpath(candidate.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
    return std::string();
}

// Resolve's executable (this code runs inside its process) — the anchor for
// finding the bundled fuscript in any install directory. Empty on failure.
std::string hostExecutablePath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // reports the needed size
    std::string buf(size + 1, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) != 0) {
        return std::string();
    }
    buf.resize(strlen(buf.c_str()));
    char resolved[PATH_MAX];
    if (realpath(buf.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
    return buf;
}

bool isExecutableFile(const std::string& path)
{
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           access(path.c_str(), X_OK) == 0;
}

// Primary interpreter: Resolve's own bundled fuscript (Contents/Libraries/
// Fusion/fuscript) — nothing to install, and immune to the OS-shipped python
// stubs of issue #34. Located from the host executable; the stock install
// path is the fallback for a standalone harness or an odd host layout.
std::string fuscriptPath()
{
    static const char* kDefaultFuscriptPath =
        "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript";
    const std::string fromHost =
        ndi_timelinewatch_mac::fuscriptPathForHostExecutable(hostExecutablePath());
    if (isExecutableFile(fromHost)) {
        return fromHost;
    }
    if (isExecutableFile(kDefaultFuscriptPath)) {
        return kDefaultFuscriptPath;
    }
    return std::string();
}

// Fallback interpreter for the Python helper, only when fuscript is missing.
// Never /usr/bin/python3: that is Apple's libxcselect shim, which without the
// Command Line Tools prints a note to stderr and exits — the silent respawn
// loop of issue #34 (and it can pop the CLT install dialog). The seam
// resolves the developer-dir python the way the shim would, then Homebrew.
std::string pythonPath()
{
    const char* developerDirEnv = getenv("DEVELOPER_DIR");
    char link[PATH_MAX];
    const ssize_t n = readlink("/var/db/xcode_select_link", link, sizeof(link) - 1);
    const std::string xcodeSelectLink = n > 0 ? std::string(link, static_cast<size_t>(n))
                                              : std::string();
    for (const std::string& candidate : ndi_timelinewatch_mac::python3CandidatePaths(
             developerDirEnv ? developerDirEnv : "", xcodeSelectLink)) {
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return std::string();
}

void setHealthLocked(bool ok, const std::string& detail)
{
    gHaveReport = ok;
    gHealthDetail = detail;
}

// Spawn the helper with its stdout on a pipe. Returns false with a health
// detail already set (caller holds gMutex).
bool spawnHelperLocked()
{
    std::vector<std::string> argv;
    std::string chain;
    const std::string fuscript = fuscriptPath();
    const std::string luaScript = bundleResourcePath("ndi_timeline_watch.lua");
    if (!fuscript.empty() && !luaScript.empty()) {
        // -q suppresses the banner fuscript writes to stdout, which the line
        // protocol would otherwise read as clip paths; -l lua selects the
        // interpreter (verified against Resolve 20's fuscript, 2026-09-01).
        argv = {fuscript, "-q", "-l", "lua", luaScript};
        chain = "fuscript [" + fuscript + "] " + luaScript;
    } else {
        // Fallback: the Python helper — a host without fuscript (standalone
        // test binaries, an exotic Resolve packaging).
        const std::string script = bundleResourcePath("ndi_timeline_watch.py");
        if (script.empty()) {
            setHealthLocked(false, "helper scripts missing from the plugin bundle's Resources");
            return false;
        }
        const std::string python = pythonPath();
        if (python.empty()) {
            setHealthLocked(false, "no fuscript in the host app bundle and no python3 (Xcode "
                                   "Command Line Tools or Homebrew) — reinstall DaVinci Resolve, "
                                   "run xcode-select --install, or set a Manual Path clip");
            return false;
        }
        argv = {python, script};
        chain = "python [" + python + "] " + script;
    }

    pid_t pid = -1;
    int fd = -1;
    std::string error;
    if (!ndi_timelinewatch_mac::spawnPipedProcess(argv, &pid, &fd, &error)) {
        setHealthLocked(false, "helper spawn failed: " + error);
        return false;
    }
    gHelperPid = pid;
    gHelperFd = fd;
    setHealthLocked(false, "helper starting");  // healthy once the first line lands
    WATCH_LOG(("helper started (" + chain + ")").c_str());
    return true;
}

// NOTE fd ownership: only the reader thread ever closes gHelperFd (via
// fclose of the FILE* wrapping it). Other threads must not close it — the
// reader may be blocked in fgets on that fd, and a cross-thread close could
// double-close an fd the host has since reused. Shutdown instead SIGTERMs
// the helper; its death EOFs the pipe and unblocks the reader within one
// heartbeat (~0.5 s).

// Reader thread: consume helper lines, dedupe, fan out changes; respawn the
// helper (with backoff) whenever it dies, until shutdown.
void readerLoop()
{
    std::string lastAnnouncedError;
    while (!gStop.load(std::memory_order_relaxed)) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (gHelperFd < 0 && !spawnHelperLocked()) {
                if (gHealthDetail != lastAnnouncedError) {
                    lastAnnouncedError = gHealthDetail;
                    WATCH_LOG(gHealthDetail.c_str());
                }
            }
            fd = gHelperFd;
        }
        if (fd < 0) {
            for (int i = 0; i < kRespawnBackoffSeconds * 10 && !gStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        FILE* stream = fdopen(fd, "r");
        if (stream == nullptr) {
            std::lock_guard<std::mutex> lock(gMutex);
            close(gHelperFd);
            gHelperFd = -1;
            if (gHelperPid > 0) {
                kill(gHelperPid, SIGTERM);
                int status = 0;
                waitpid(gHelperPid, &status, 0);
                gHelperPid = -1;
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
                    WATCH_LOG(msg.c_str());
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
                WATCH_LOG((changed.empty() ? std::string("playhead clip: (none)")
                                           : "playhead clip: '" + changed + "'")
                              .c_str());
                if (onChange) {
                    onChange(changed);
                }
            }
        }
        // EOF (helper died) or shutdown. fclose also closes the fd.
        fclose(stream);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gHelperFd = -1;
            // The exit status is how a dying helper identifies itself — a
            // stub interpreter dies before its first protocol line, so it is
            // the only evidence that ever reaches the log (issue #34).
            std::string exitDetail = "status unknown";
            if (gHelperPid > 0) {
                int status = 0;
                if (waitpid(gHelperPid, &status, 0) == gHelperPid) {
                    exitDetail = ndi_timelinewatch_mac::describeWaitStatus(status);
                }
                gHelperPid = -1;
            }
            if (!gStop.load(std::memory_order_relaxed)) {
                setHealthLocked(false, "helper exited (" + exitDetail + ") — retrying");
                WATCH_LOG(("helper exited (" + exitDetail + ") — retrying in 30 s (Manual "
                           "Path mode is unaffected)").c_str());
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
        // Killing the helper EOFs the pipe, which unblocks the reader's
        // fgets within one heartbeat; the reader owns the fd (see above).
        if (gHelperPid > 0) {
            kill(gHelperPid, SIGTERM);
        }
    }
    if (gThread.joinable()) {
        gThread.join();
    }
    std::lock_guard<std::mutex> lock(gMutex);
    if (gHelperPid > 0) {  // reader exited before reaping (spawn-window race)
        int status = 0;
        waitpid(gHelperPid, &status, 0);
        gHelperPid = -1;
    }
    gStarted = false;
    gOnChange = nullptr;
    setHealthLocked(false, "stopped");
}

} // namespace ndi_timelinewatch

#endif // __APPLE__
