#!/usr/bin/env python3
"""Timeline clip watcher helper (issue #11, auto camera-clip mode).

Spawned by the NDI Output plugin FROM INSIDE the Resolve process (so Resolve
is our parent). Polls the Resolve scripting API for the media file under the
playhead of the current timeline and prints it to stdout — one line per poll
(~2/s), empty line when nothing resolvable is under the playhead. Printing
every poll (not just changes) doubles as a liveness heartbeat.

Protocol (read by src/TimelineClipWatcher.cpp):
  - a line starting with '!' is a status/error message for the plugin log;
  - any other line is the current clip's file path ('' = none).

Exits on its own when Resolve dies (reparented to launchd) or on SIGTERM from
the plugin. Requires Resolve's external scripting preference (DaVinci Resolve
> Preferences > System > General > External scripting using: Local) and a
python3 whose ABI the bundled fusionscript.dylib accepts (the macOS system
/usr/bin/python3 works).
"""
import os
import sys
import time

MODULES = ("/Library/Application Support/Blackmagic Design/DaVinci Resolve/"
           "Developer/Scripting/Modules")
sys.path.append(MODULES)

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
    if os.getppid() == 1:  # Resolve (our parent) is gone
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
