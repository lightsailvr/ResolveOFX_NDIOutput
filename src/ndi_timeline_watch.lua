-- Timeline clip watcher helper, Lua flavor (one-click Timeline (Auto);
-- supersedes the Python helper as the primary path on macOS and Windows).
--
-- Runs under Resolve's OWN bundled script interpreter (fuscript: macOS
-- Contents/Libraries/Fusion/fuscript, Windows fuscript.exe beside
-- Resolve.exe), so it needs no Python - or anything else - installed. The
-- Python helper (ndi_timeline_watch.py) remains the documented fallback for
-- hosts without fuscript. Why not Python: both OSes ship a python at the
-- canonical path that is not one (issue #34 - Apple's /usr/bin/python3 CLT
-- shim, the Windows Store alias stub), and on Windows Resolve's
-- fusionscript.dll only binds a PEP 514 registry-REGISTERED interpreter -
-- never the process that loaded it - so a bundled/embedded Python can never
-- work (LEARNINGS 2026-09-01, the fusionscript registry findings).
--
-- Protocol (read by src/TimelineClipWatcher.cpp on macOS and
-- src/WinTimelineWatch.cpp on Windows), identical to the Python helper: one
-- line per poll (~2/s); a line starting with '!' is a status message for the
-- plugin log; any other line is the current clip's file path ('' = none).
-- Printing every poll doubles as a liveness heartbeat. Exits when the reader
-- hangs up (stdout write fails => plugin/Resolve gone).

local POLL_SECONDS = 0.5

local function say(line)
  local ok = io.stdout:write(line, "\n")
  if not ok then os.exit(0) end   -- reader hung up: plugin is gone
  io.stdout:flush()
end

local function sleep(seconds)
  if bmd and bmd.wait then
    bmd.wait(seconds)
  else
    local deadline = os.clock() + seconds  -- crude, but only if bmd.wait vanishes
    while os.clock() < deadline do end
  end
end

if not (bmd and bmd.scriptapp) then
  say("!fuscript provided no bmd.scriptapp - cannot reach Resolve")
  os.exit(1)
end

local resolve = nil
local warned_no_app = false
while true do
  local path = ""
  local ok, err = pcall(function()
    if resolve == nil then
      resolve = bmd.scriptapp("Resolve")
      if resolve == nil and not warned_no_app then
        warned_no_app = true
        say("!scriptapp('Resolve') returned nothing - is external scripting " ..
            "enabled? (Preferences > System > General)")
      end
    end
    if resolve ~= nil then
      local pm = resolve:GetProjectManager()
      local project = pm and pm:GetCurrentProject() or nil
      local timeline = project and project:GetCurrentTimeline() or nil
      local item = timeline and timeline:GetCurrentVideoItem() or nil
      local pool = item and item:GetMediaPoolItem() or nil
      if pool then
        path = pool:GetClipProperty("File Path")
        if type(path) ~= "string" then path = "" end
      end
    end
  end)
  if not ok then
    -- Stale object across a project/timeline switch - reconnect next poll.
    resolve = nil
    say("!query failed (will reconnect): " .. tostring(err))
    path = ""
  end
  say(path)
  sleep(POLL_SECONDS)
end
