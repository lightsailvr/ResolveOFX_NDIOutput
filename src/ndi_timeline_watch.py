#!/usr/bin/env python3
"""Timeline clip watcher helper (issue #11, auto camera-clip mode).

Spawned by the NDI Output plugin FROM INSIDE the Resolve process (so Resolve
is our parent). Polls the Resolve scripting API for the media file under the
playhead of the current timeline and prints it to stdout — one line per poll
(~2/s), empty line when nothing resolvable is under the playhead. Printing
every poll (not just changes) doubles as a liveness heartbeat.

Protocol (read by src/TimelineClipWatcher.cpp on macOS and
src/WinTimelineWatch.cpp on Windows):
  - a line starting with '!' is a status/error message for the plugin log;
  - any other line is the current clip's file path ('' = none).

argv[1] (Windows, optional): full path of Resolve's fusionscript.dll,
derived by the plugin from the running Resolve.exe so non-default install
directories work; exported as RESOLVE_SCRIPT_LIB for DaVinciResolveScript's
own bootstrap. macOS spawns with no arguments (the dylib default is fine).

Exits on its own when Resolve dies — macOS: reparented to launchd (getppid
check); Windows: the plugin's pipe read handle dies with Resolve, so the
next heartbeat write fails — or when terminated by the plugin. Requires
Resolve's external scripting preference (DaVinci Resolve > Preferences >
System > General > External scripting using: Local) and a python3 the host's
fusionscript accepts (macOS: the system /usr/bin/python3 works; Windows: a
64-bit Python 3 — see BUILD.md, Windows section).
"""
import os
import sys
import time

IS_WINDOWS = sys.platform.startswith("win")
if IS_WINDOWS:
    MODULES = os.path.join(
        os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
        "Blackmagic Design", "DaVinci Resolve", "Support", "Developer",
        "Scripting", "Modules")
else:
    MODULES = ("/Library/Application Support/Blackmagic Design/DaVinci Resolve/"
               "Developer/Scripting/Modules")
sys.path.append(MODULES)

if len(sys.argv) > 1 and sys.argv[1]:
    os.environ.setdefault("RESOLVE_SCRIPT_LIB", sys.argv[1])

POLL_SECONDS = 0.5


def say(line):
    try:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
    except Exception:
        sys.exit(0)  # reader hung up: plugin is gone


try:
    import DaVinciResolveScript as dvr
except Exception as exc:  # module missing / fusionscript ABI mismatch
    say("!cannot import DaVinciResolveScript from '%s': %s" % (MODULES, exc))
    sys.exit(1)

resolve = None
warned_no_app = False
while True:
    if not IS_WINDOWS and os.getppid() == 1:  # Resolve (our parent) is gone
        sys.exit(0)
    path = ""
    try:
        if resolve is None:
            resolve = dvr.scriptapp("Resolve")
            if resolve is None and not warned_no_app:
                warned_no_app = True
                say("!scriptapp('Resolve') returned nothing — is external "
                    "scripting enabled? (Preferences > System > General)")
        if resolve is not None:
            pm = resolve.GetProjectManager()
            project = pm.GetCurrentProject() if pm else None
            timeline = project.GetCurrentTimeline() if project else None
            item = timeline.GetCurrentVideoItem() if timeline else None
            pool_item = item.GetMediaPoolItem() if item else None
            if pool_item:
                path = pool_item.GetClipProperty("File Path") or ""
    except Exception as exc:
        # Stale object across a project/timeline switch — reconnect next poll.
        resolve = None
        say("!query failed (will reconnect): %s" % exc)
        path = ""
    say(path)
    time.sleep(POLL_SECONDS)
