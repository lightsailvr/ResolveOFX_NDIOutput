# Install the staged Windows bundle into Resolve's OFX plugin directory
# (Tier 1 of the Windows testing loop - BUILD.md, Windows section).
#
#   .\scripts\install_windows.ps1 [-Stage .\stage] [-ResetCache]
#
# MUST run from an elevated PowerShell (right-click -> Run as administrator):
# Common Files needs admin, and UAC prompts spawned from agent/automation
# shells on this machine get auto-canceled, so the script never tries to
# self-elevate. Run `cmake --install build --config Release --prefix stage`
# first to produce the staged bundle.

param(
    [string]$Stage = ".\stage",
    [switch]$ResetCache
)

$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error ("Not elevated. Open PowerShell with 'Run as administrator' " +
        "and re-run (UAC prompts from automation shells auto-cancel on this machine).")
    exit 1
}

$bundle = Join-Path $Stage "NDIOutput.ofx.bundle"
$binDir = Join-Path $bundle "Contents\Win64"
$ofx = Join-Path $binDir "NDIOutput.ofx"
$ndiDll = Join-Path $binDir "Processing.NDI.Lib.Advanced.x64.dll"

if (-not (Test-Path $ofx)) {
    Write-Error ("No staged bundle at $ofx. Run: cmake --install build " +
        "--config Release --prefix stage")
    exit 1
}
if (-not (Test-Path $ndiDll)) {
    Write-Warning ("$ndiDll is missing - this is a stub-linked CI-style build " +
        "and will NOT stream. Build on a machine with the NDI Advanced SDK installed.")
}

if (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue) {
    Write-Error "DaVinci Resolve is running. Quit it fully first (no OFX hot reload)."
    exit 1
}

$pluginsDir = "C:\Program Files\Common Files\OFX\Plugins"
$dest = Join-Path $pluginsDir "NDIOutput.ofx.bundle"
if (-not (Test-Path $pluginsDir)) {
    New-Item -ItemType Directory -Force $pluginsDir | Out-Null
}
if (Test-Path $dest) {
    Remove-Item -Recurse -Force $dest
}
Copy-Item -Recurse $bundle $dest
Write-Host "Installed: $dest"

# The plugin cache re-scans on binary mtime/size change; -ResetCache forces a
# full re-scan for when the plugin vanished from the Effects Library anyway.
$cache = Join-Path $env:APPDATA "Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml"
if ($ResetCache) {
    if (Test-Path $cache) {
        Remove-Item -Force $cache
        Write-Host "Plugin cache deleted: $cache"
    } else {
        Write-Host "Plugin cache already absent: $cache"
    }
}

Write-Host ""
Write-Host "Next (Tier 1-2, human): start Resolve, add the NDI Output effect,"
Write-Host "press play, and verify the stream in NDI Studio Monitor on a second"
Write-Host "machine. Expect a Windows Firewall prompt on the FIRST send - allow"
Write-Host "on private networks, or the stream stays invisible off-machine."
