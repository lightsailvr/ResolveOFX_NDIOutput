#ifndef _TimelineClipWatcher_h_
#define _TimelineClipWatcher_h_

/*
  Playhead clip watcher (issue #11, "Timeline (Auto)" camera-clip source):
  keeps a process-wide answer to "which media file is under the Resolve
  playhead right now?" so the camera-metadata projection can follow the
  timeline across multi-camera edits without a manual pick.

  Mechanism: Resolve's scripting API is only reachable from a script
  interpreter (fusionscript), so the plugin spawns ONE long-running helper
  that polls GetCurrentTimeline().GetCurrentVideoItem() ~2x/s and prints the
  clip path per poll; a reader thread here dedupes and fans changes out
  through the registered callback. The helper is src/ndi_timeline_watch.lua
  under Resolve's OWN bundled interpreter (`fuscript -q -l lua`: macOS
  Contents/Libraries/Fusion/fuscript, Windows fuscript.exe beside Resolve.exe)
  — nothing to install (issue #34: both OSes ship a python at the canonical
  path that isn't one — Apple's CLT shim, the Store alias stub — which spawns
  and dies silently). The Python helper (src/ndi_timeline_watch.py) remains
  the fallback when fuscript is missing (macOS: developer-dir or Homebrew
  python3, never the shim; Windows: PEP 514 registry then PATH — see
  BUILD.md). Both ride in Contents/Resources. Requires Resolve Studio's external-scripting
  preference; everything fails soft into "no path" + a log line (a dead
  helper's exit status included), and manual mode remains available. 

  Per-platform implementations of this one API (ticket #25): macOS
  posix_spawn/pipe in src/TimelineClipWatcher.cpp, Windows CreateProcessW/
  anonymous pipe in src/WinTimelineWatch.cpp (spawn + discovery seam in
  src/WinTimelineWatch.h; the macOS seam is src/MacTimelineWatch.h). NDI_TIMELINE_WATCH gates the plugin's call sites,
  replacing the __APPLE__ checks from the macOS-only era.

  Threading: the change callback fires on the watcher's reader thread —
  callees synchronize their own state (the plugin swaps maps under the same
  mutex the render path copies them under). All entry points are safe to
  call from the host main thread; ensureStarted is idempotent and latches
  the first callback it is given.
*/

#if defined(__APPLE__) || defined(_WIN32)
#define NDI_TIMELINE_WATCH 1
#endif

#ifdef NDI_TIMELINE_WATCH

#include <functional>
#include <string>

namespace ndi_timelinewatch {

// Start the helper + reader thread (idempotent; the first call's callback
// wins). The callback fires on the reader thread with the deduped current
// clip path — possibly "" (nothing under the playhead / scripting down).
void ensureStarted(std::function<void(const std::string&)> onChange);

// Last path the helper reported ("" until the first report arrives).
std::string currentClipPath();

// False until the helper produces its first report, or after it dies (it is
// respawned with backoff). *detail carries a human-readable reason for logs.
bool healthy(std::string* detail);

// Stop the helper and reader thread (plugin unload).
void shutdown();

} // namespace ndi_timelinewatch

#endif // NDI_TIMELINE_WATCH

#endif
