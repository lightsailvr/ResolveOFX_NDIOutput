// Tests for the macOS timeline-watch support seam (src/MacTimelineWatch.h):
// fuscript location from the host executable, the python3 fallback's
// candidate order (which must never include Apple's /usr/bin/python3 shim —
// issue #34), wait-status wording for the helper-exit log, and the
// posix_spawn/pipe plumbing smoke-tested with /bin/sh. Build & run: make test.

#include "MacTimelineWatch.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

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

static void testFuscriptPathForHostExecutable()
{
    using ndi_timelinewatch_mac::fuscriptPathForHostExecutable;

    // The stock install: Resolve's executable in Contents/MacOS, fuscript
    // two levels over in Contents/Libraries/Fusion (verified on disk,
    // Resolve 20, 2026-09-01).
    expectTrue(fuscriptPathForHostExecutable(
                   "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/MacOS/Resolve") ==
                   "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript",
               "stock install resolves to Contents/Libraries/Fusion/fuscript");

    // A relocated app bundle keeps the same internal layout.
    expectTrue(fuscriptPathForHostExecutable("/Volumes/Apps/R20/DaVinci Resolve.app/Contents/MacOS/Resolve") ==
                   "/Volumes/Apps/R20/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript",
               "relocated bundle keeps the internal layout");

    // Too shallow to hop from <exe dir> up to Contents: empty (caller falls
    // back to the default install path, then to python).
    expectTrue(fuscriptPathForHostExecutable("Resolve").empty(), "bare name yields empty");
    expectTrue(fuscriptPathForHostExecutable("MacOS/Resolve").empty(),
               "single directory level yields empty");
    expectTrue(fuscriptPathForHostExecutable("").empty(), "empty path yields empty");
}

static bool contains(const std::vector<std::string>& v, const std::string& s)
{
    for (const std::string& x : v) {
        if (x == s) return true;
    }
    return false;
}

static void testPython3CandidatePaths()
{
    using ndi_timelinewatch_mac::python3CandidatePaths;

    // /usr/bin/python3 is Apple's libxcselect shim: without the Command Line
    // Tools it prints "xcode-select: note: No developer tools were found" and
    // exits (and can pop the CLT install dialog) — the silent respawn loop of
    // issue #34. The watcher must resolve the developer-dir python itself,
    // in xcode-select's own precedence, and never spawn the shim.
    {
        const std::vector<std::string> c = python3CandidatePaths(
            "", "/Applications/Xcode.app/Contents/Developer");
        expectTrue(!contains(c, "/usr/bin/python3"), "the /usr/bin/python3 shim is never a candidate");
        expectTrue(!c.empty() && c[0] == "/Applications/Xcode.app/Contents/Developer/usr/bin/python3",
                   "xcode-select's developer dir comes first");
        expectTrue(contains(c, "/Library/Developer/CommandLineTools/usr/bin/python3"),
                   "the CLT default location is tried");
        expectTrue(contains(c, "/opt/homebrew/bin/python3") && contains(c, "/usr/local/bin/python3"),
                   "Homebrew (arm64 and Intel prefixes) are tried");
        expectTrue(c.back() == "/usr/local/bin/python3",
                   "Homebrew paths come after the developer-tool pythons (fusionscript verified against the CLT 3.9)");
    }
    // DEVELOPER_DIR overrides xcode-select (libxcselect's precedence).
    {
        const std::vector<std::string> c = python3CandidatePaths(
            "/Users/x/Xcode-beta.app/Contents/Developer", "/Applications/Xcode.app/Contents/Developer");
        expectTrue(c.size() >= 2 && c[0] == "/Users/x/Xcode-beta.app/Contents/Developer/usr/bin/python3" &&
                       c[1] == "/Applications/Xcode.app/Contents/Developer/usr/bin/python3",
                   "DEVELOPER_DIR outranks the xcode-select link");
    }
    // No env, no link (a machine that never ran xcode-select): the fixed
    // Xcode and CLT locations still get a look, in that order.
    {
        const std::vector<std::string> c = python3CandidatePaths("", "");
        expectTrue(c.size() >= 2 && c[0] == "/Applications/Xcode.app/Contents/Developer/usr/bin/python3" &&
                       c[1] == "/Library/Developer/CommandLineTools/usr/bin/python3",
                   "empty env and link fall through to Xcode.app then CLT");
    }
    // The same developer dir arriving twice yields one candidate.
    {
        const std::vector<std::string> c = python3CandidatePaths(
            "/Library/Developer/CommandLineTools", "/Library/Developer/CommandLineTools");
        int n = 0;
        for (const std::string& x : c) {
            if (x == "/Library/Developer/CommandLineTools/usr/bin/python3") ++n;
        }
        expectTrue(n == 1, "duplicate developer dirs collapse to one candidate");
    }
    // A trailing slash on the link (readlink can hand one back) is tolerated.
    {
        const std::vector<std::string> c = python3CandidatePaths("", "/Applications/Xcode.app/Contents/Developer/");
        expectTrue(!c.empty() && c[0] == "/Applications/Xcode.app/Contents/Developer/usr/bin/python3",
                   "trailing slash on the developer dir is normalized");
    }
}

static void testDescribeWaitStatus()
{
    using ndi_timelinewatch_mac::describeWaitStatus;

    // waitpid status encoding (sys/wait.h): a normal exit stores the code in
    // the high byte; a signal death stores the signal number in the low bits.
    // The exit code is what identifies a stub — Windows' 9009 did — so it
    // must reach the log verbatim.
    expectTrue(describeWaitStatus(0) == "code 0", "clean exit reads as code 0");
    expectTrue(describeWaitStatus(1 << 8) == "code 1", "exit(1) reads as code 1");
    expectTrue(describeWaitStatus(127 << 8) == "code 127", "exit(127) (exec failure) reads as code 127");
    expectTrue(describeWaitStatus(15) == "signal 15", "SIGTERM death reads as signal 15");
    expectTrue(describeWaitStatus(9) == "signal 9", "SIGKILL death reads as signal 9");
}

static void testSpawnPipedProcess()
{
    using ndi_timelinewatch_mac::describeWaitStatus;
    using ndi_timelinewatch_mac::spawnPipedProcess;

    // Spawn a real child with its stdout on our pipe and read it back through
    // the same FILE* wrapping the watcher uses: lines arrive intact, the
    // child's exit EOFs the pipe (no stray write end kept on our side), and
    // its exit code survives to the log. stderr must NOT reach the pipe —
    // the line protocol would read it as clip paths.
    pid_t pid = -1;
    int fd = -1;
    std::string error;
    const std::vector<std::string> argv = {
        "/bin/sh", "-c", "printf '!status\\n/Volumes/clip.braw\\n'; echo noise 1>&2; exit 7"};
    expectTrue(spawnPipedProcess(argv, &pid, &fd, &error),
               ("spawn succeeds" + (error.empty() ? "" : " (" + error + ")")).c_str());
    if (pid <= 0) {
        return;
    }
    // Both pipe ends must be close-on-exec: this spawn happens inside
    // Resolve's process, and any OTHER child Resolve forks meanwhile would
    // otherwise inherit the write end and keep the pipe open past the
    // helper's death — the watcher's only wake-up signal would never fire.
    expectTrue((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "read end is close-on-exec");
    FILE* stream = fdopen(fd, "r");
    expectTrue(stream != nullptr, "read fd wraps into a FILE*");
    bool sawStatus = false, sawPath = false, sawNoise = false;
    if (stream) {
        char line[256];
        while (fgets(line, sizeof(line), stream) != nullptr) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s == "!status") sawStatus = true;
            if (s == "/Volumes/clip.braw") sawPath = true;
            if (s == "noise") sawNoise = true;
        }
        fclose(stream);
    } else {
        close(fd);
    }
    expectTrue(sawStatus, "first line arrives intact");
    expectTrue(sawPath, "second line arrives intact");
    expectTrue(!sawNoise, "stderr does not leak into the line protocol");
    int status = 0;
    expectTrue(waitpid(pid, &status, 0) == pid, "child is reapable after EOF");
    expectTrue(describeWaitStatus(status) == "code 7", "exit code survives to the log wording");

    // A missing executable fails honestly, with errno wording, and hands back
    // nothing to close.
    pid = -1;
    fd = -1;
    error.clear();
    expectTrue(!spawnPipedProcess({"/nonexistent/fuscript", "-q"}, &pid, &fd, &error),
               "missing executable fails");
    expectTrue(!error.empty() && pid == -1 && fd == -1,
               "failure reports an error and leaves no pid/fd behind");
}

static void testExitsCleanly()
{
    using ndi_timelinewatch_mac::exitsCleanly;

    // Probe-once for the python fallback (issue #34): an interpreter is only
    // trusted after it has been seen to run `--version` and exit 0 — a stub
    // at any path (not just Apple's shim) exits non-zero and is skipped.
    expectTrue(exitsCleanly({"/bin/sh", "-c", "echo Python 3.9.6; exit 0"}),
               "a candidate that exits 0 is trusted");
    expectTrue(!exitsCleanly({"/bin/sh", "-c", "echo note: no developer tools 1>&2; exit 1"}),
               "a candidate that exits non-zero is rejected");
    expectTrue(!exitsCleanly({"/nonexistent/python3", "--version"}),
               "a candidate that cannot spawn is rejected");
    expectTrue(!exitsCleanly({"/bin/sh", "-c", "kill -TERM $$"}),
               "a candidate that dies by signal is rejected");
}

int main()
{
    testFuscriptPathForHostExecutable();
    testPython3CandidatePaths();
    testDescribeWaitStatus();
    testSpawnPipedProcess();
    testExitsCleanly();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all mac timeline-watch tests passed\n");
    return 0;
}
