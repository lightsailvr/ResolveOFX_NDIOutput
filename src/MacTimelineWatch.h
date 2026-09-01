// Support seam for the macOS timeline-clip watcher (issue #34): the pure
// pieces of "find Resolve's own script interpreter, pick a python3 that is
// actually an interpreter, describe a dead child" live here so the unit
// suite (tests/test_mac_timeline_watch.cpp) exercises them without Resolve,
// mirroring src/WinTimelineWatch.h on the Windows port. The watcher itself —
// reader thread, respawn/backoff, the line protocol — is
// src/TimelineClipWatcher.cpp, implementing the TimelineClipWatcher.h API.

#ifndef MAC_TIMELINE_WATCH_H
#define MAC_TIMELINE_WATCH_H

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace ndi_timelinewatch_mac {

// ---- fuscript location -------------------------------------------------------
// The primary watcher interpreter is Resolve's OWN bundled `fuscript`
// (Contents/Libraries/Fusion/fuscript, verified on Resolve 20) running the Lua
// helper — nothing to install, and no dependence on whichever python3 the OS
// pretends to ship. From the host executable (Contents/MacOS/Resolve — this
// code runs inside Resolve's process) hop up to Contents and over. Empty when
// the path is too shallow to hop (caller tries the default install path, then
// the python fallback).
inline std::string fuscriptPathForHostExecutable(const std::string& hostExecutable)
{
    const size_t last = hostExecutable.find_last_of('/');
    if (last == std::string::npos || last == 0) {
        return std::string();
    }
    const size_t contents = hostExecutable.find_last_of('/', last - 1);
    if (contents == std::string::npos) {
        return std::string();
    }
    return hostExecutable.substr(0, contents + 1) + "Libraries/Fusion/fuscript";
}

// ---- python3 fallback candidates -----------------------------------------------
// Only reached when fuscript is missing beside the host. /usr/bin/python3 is
// NOT a python: it is Apple's libxcselect shim, which execs
// <developer dir>/usr/bin/python3 when the Command Line Tools are installed
// and otherwise prints "xcode-select: note: No developer tools were found"
// (to stderr, which the watcher discards) and exits — the silent respawn loop
// of issue #34. So resolve the developer dir ourselves, in libxcselect's own
// precedence (DEVELOPER_DIR, then the xcode-select link at
// /var/db/xcode_select_link, then Xcode.app, then the CLT default), and spawn
// that python directly; Homebrew's prefixes follow. Pure: callers pass the
// env value and the readlink result. Missing entries are simply skipped;
// existence on disk is the caller's check.
inline std::vector<std::string> python3CandidatePaths(const std::string& developerDirEnv,
                                                      const std::string& xcodeSelectLink)
{
    std::vector<std::string> out;
    auto addDeveloperDir = [&out](std::string dir) {
        if (dir.empty()) {
            return;
        }
        while (dir.size() > 1 && dir.back() == '/') {
            dir.pop_back();
        }
        const std::string python = dir + "/usr/bin/python3";
        for (const std::string& seen : out) {
            if (seen == python) {
                return;
            }
        }
        out.push_back(python);
    };
    addDeveloperDir(developerDirEnv);
    addDeveloperDir(xcodeSelectLink);
    addDeveloperDir("/Applications/Xcode.app/Contents/Developer");
    addDeveloperDir("/Library/Developer/CommandLineTools");
    out.push_back("/opt/homebrew/bin/python3");
    out.push_back("/usr/local/bin/python3");
    return out;
}

// ---- dead-child description ------------------------------------------------------
// One phrase for the helper-exit log from a waitpid() status: "code N" for a
// normal exit, "signal N" for a signal death. The exit code is what
// identifies an interpreter that is really a stub (the Windows watcher's
// 9009 did), so it always reaches the log — issue #34.
inline std::string describeWaitStatus(int waitStatus)
{
    if (WIFEXITED(waitStatus)) {
        return "code " + std::to_string(WEXITSTATUS(waitStatus));
    }
    if (WIFSIGNALED(waitStatus)) {
        return "signal " + std::to_string(WTERMSIG(waitStatus));
    }
    return "status " + std::to_string(waitStatus);
}

// ---- Process spawn -----------------------------------------------------------------
// posix_spawn with the child's stdout on a fresh pipe and stdin/stderr on
// /dev/null (stderr must never join the pipe — the line protocol would read
// it as clip paths; the child's exit code is how a dying helper identifies
// itself instead). Our copy of the write end is closed before returning, so
// the child's death is guaranteed to EOF the read end — the watcher's only
// wake-up signal. On success *pid and *readFd are the caller's to reap and
// close; on failure both are -1 and *error carries errno wording.
inline bool spawnPipedProcess(const std::vector<std::string>& argv, pid_t* pid, int* readFd,
                              std::string* error)
{
    *pid = -1;
    *readFd = -1;
    auto fail = [error](const char* what, int err) {
        if (error) {
            *error = std::string(what) + ": " + strerror(err);
        }
        return false;
    };
    if (argv.empty()) {
        return fail("spawn", EINVAL);
    }

    int fds[2];
    if (pipe(fds) != 0) {
        return fail("pipe() failed", errno);
    }
    // Close-on-exec on both ends: this runs inside Resolve's process, and any
    // OTHER child Resolve forks meanwhile would otherwise inherit the write
    // end and hold the pipe open past the helper's death. The dup2 below
    // gives the helper itself a plain (inheritable) stdout.
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);

    pid_t child = -1;
    const int rc = posix_spawn(&child, argv[0].c_str(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);  // the child holds its own copy now (or never will)
    if (rc != 0) {
        close(fds[0]);
        return fail("posix_spawn failed", rc);
    }
    *pid = child;
    *readFd = fds[0];
    return true;
}

// ---- Probe-once ---------------------------------------------------------------------
// Run a candidate to completion with its output discarded; true only for a
// clean exit(0). The python fallback trusts an interpreter only after seeing
// it run `--version` — hardening against ANY stub at a canonical path, not
// just the two known today (issue #34).
inline bool exitsCleanly(const std::vector<std::string>& argv)
{
    pid_t pid = -1;
    int fd = -1;
    std::string error;
    if (!spawnPipedProcess(argv, &pid, &fd, &error)) {
        return false;
    }
    char sink[256];
    while (read(fd, sink, sizeof(sink)) > 0) {
    }
    close(fd);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace ndi_timelinewatch_mac

#endif // MAC_TIMELINE_WATCH_H
