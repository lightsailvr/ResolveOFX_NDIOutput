// Implementation of src/TimelineClipWatcher.h — see that header for the
// design. One helper process + one reader thread per plugin load.
#ifdef __APPLE__

#include "TimelineClipWatcher.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <libgen.h>
#include <os/log.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#define WATCH_LOG(str) os_log(OS_LOG_DEFAULT, "NDI Plugin: TimelineWatch: %{public}s", str)

extern char** environ;

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

std::string pythonPath()
{
    static const char* kCandidates[] = {
        "/usr/bin/python3",
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
    };
    for (const char* candidate : kCandidates) {
        struct stat st;
        if (stat(candidate, &st) == 0 && (st.st_mode & S_IXUSR)) {
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
    const std::string script = bundleResourcePath("ndi_timeline_watch.py");
    if (script.empty()) {
        setHealthLocked(false, "helper script missing from the plugin bundle's Resources");
        return false;
    }
    const std::string python = pythonPath();
    if (python.empty()) {
        setHealthLocked(false, "no python3 found (/usr/bin/python3) — install the "
                               "Xcode Command Line Tools or set a Manual Path clip");
        return false;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        setHealthLocked(false, "pipe() failed");
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    const char* argv[] = {python.c_str(), script.c_str(), nullptr};
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, python.c_str(), &actions, nullptr,
                               const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        setHealthLocked(false, std::string("posix_spawn failed: ") + strerror(rc));
        return false;
    }
    gHelperPid = pid;
    gHelperFd = fds[0];
    setHealthLocked(false, "helper starting");  // healthy once the first line lands
    WATCH_LOG(("helper started (" + python + " " + script + ")").c_str());
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
            if (gHelperPid > 0) {
                int status = 0;
                waitpid(gHelperPid, &status, 0);
                gHelperPid = -1;
            }
            if (!gStop.load(std::memory_order_relaxed)) {
                setHealthLocked(false, "helper exited — retrying");
                WATCH_LOG("helper exited — retrying in 30 s (Manual Path mode is "
                          "unaffected)");
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
