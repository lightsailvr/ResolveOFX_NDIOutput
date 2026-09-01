# Installer acceptance test (ticket #23): drive the built Inno Setup installer
# through a full silent install -> verify -> silent uninstall -> verify-clean
# cycle. This is the automated half of the ticket's acceptance criteria
# ("/VERYSILENT install works", "uninstall removes the bundle cleanly",
# "attribution present beside the binaries"); the human half (fresh machine,
# Resolve restart, stream in Studio Monitor) stays Tier 1-2 in BUILD.md.
#
#   powershell -ExecutionPolicy Bypass -File .\scripts\test_windows_installer.ps1
#
# MUST run elevated: it installs into C:\Program Files\Common Files. CI runs it
# on the windows-2022 runner (already elevated); on the workstation, open an
# elevated PowerShell yourself (UAC prompts from automation shells auto-cancel).
#
# It refuses to run when a bundle is already installed - it would uninstall
# somebody's dev install at the end. Pass -Force to accept that.

param(
    [string]$Installer = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo "VERSION") -Raw).Trim()

if ($Installer -eq "") {
    # A real-SDK build if one was packaged, otherwise the stub-linked build CI
    # produces - the install/uninstall contract is identical either way, but a
    # real build additionally has to carry the NDI runtime and its licenses.
    $candidates = @("NDIOutput-$version-Windows-x64.exe",
                    "NDIOutput-$version-Windows-x64-STUB.exe") |
                  ForEach-Object { Join-Path $repo "dist\v$version\$_" }
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    # Falling back to the first candidate makes the "not found" error name the
    # artifact a release build would have produced.
    $Installer = if ($found) { $found } else { $candidates[0] }
}
if (-not (Test-Path $Installer)) {
    Write-Error ("No installer at $Installer - build one first: " +
        "powershell -ExecutionPolicy Bypass -File .\scripts\package_windows_release.ps1 -AllowStub")
    exit 1
}
$Installer = (Resolve-Path $Installer).Path

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Not elevated. Installing into Common Files needs administrator."
    exit 1
}
if (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue) {
    Write-Error "DaVinci Resolve is running - the installer refuses to touch a loaded plugin."
    exit 1
}

$bundle = "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle"
if ((Test-Path $bundle) -and -not $Force) {
    Write-Error ("$bundle already exists. This test uninstalls what it installs, " +
        "which would remove it. Remove it yourself or pass -Force.")
    exit 1
}

$failures = New-Object System.Collections.Generic.List[string]
function Check($what, [bool]$ok) {
    if ($ok) { Write-Host "  [ok]   $what" }
    else { Write-Host "  [FAIL] $what"; $failures.Add($what) }
}

# ------------------------------------------------------------- ARP lookup ----
# The Add-or-Remove-Programs entry Inno registers for the installed AppId.
function Get-ArpEntry {
    $roots = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($key in Get-ChildItem $root) {
            # Unreadable third-party keys must not abort the run ($ErrorActionPreference = Stop).
            $props = Get-ItemProperty $key.PSPath -ErrorAction SilentlyContinue
            if ($props -and $props.DisplayName -like "NDI Output for DaVinci Resolve*") { return $props }
        }
    }
    return $null
}

# ------------------------------------------- refusal while Resolve runs ----
# The fleet-deployment contract documented in README.md and the release notes:
# with Resolve running, a silent install exits non-zero and installs nothing
# rather than touching a loaded plugin. The installer looks Resolve up by image
# name, so a copy of ping.exe named Resolve.exe stands in for it.
$fakeDir = Join-Path $env:TEMP "ndi-fake-resolve"
$fake = Join-Path $fakeDir "Resolve.exe"
New-Item -ItemType Directory -Force $fakeDir | Out-Null
Copy-Item (Join-Path $env:SystemRoot "System32\PING.EXE") $fake -Force
$fakeProc = Start-Process -FilePath $fake -ArgumentList "-t", "127.0.0.1" `
    -PassThru -WindowStyle Hidden
try {
    Write-Host "Silent install with a running 'Resolve.exe' (must refuse):"
    $r = Start-Process -FilePath $Installer -Wait -PassThru `
        -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"
    Check "install refused while Resolve runs (exit $($r.ExitCode), expected non-zero)" `
        ($r.ExitCode -ne 0)
    Check "refused install left nothing behind" (-not (Test-Path $bundle))
} finally {
    Stop-Process -Id $fakeProc.Id -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $fakeProc.Id -Timeout 15 -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $fakeDir -ErrorAction SilentlyContinue
}

# ----------------------------------------------------------------- install ----
$log = Join-Path $env:TEMP "ndioutput-install.log"
Write-Host "Silent install: $Installer"
$p = Start-Process -FilePath $Installer -Wait -PassThru `
    -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=$log"
Check "/VERYSILENT install exits 0 (got $($p.ExitCode))" ($p.ExitCode -eq 0)

$ofx = Join-Path $bundle "Contents\Win64\NDIOutput.ofx"
$readme = Join-Path $bundle "Contents\Resources\README.txt"
$watcherLua = Join-Path $bundle "Contents\Resources\ndi_timeline_watch.lua"
$watcher = Join-Path $bundle "Contents\Resources\ndi_timeline_watch.py"
Check "plugin at Contents\Win64\NDIOutput.ofx" (Test-Path $ofx)
Check "Lua watcher helper (primary, fuscript) staged" (Test-Path $watcherLua)
Check "Python watcher helper (fallback) staged" (Test-Path $watcher)
Check "attribution readme beside the payload" (Test-Path $readme)

# A release build must additionally land the NDI runtime and the third-party
# licenses file beside the binary (spec decision 13). A -STUB installer was
# packaged without an SDK by definition, so those are not expected there.
$isStubBuild = (Split-Path -Leaf $Installer) -like "*-STUB.exe"
if ($isStubBuild) {
    Write-Host "  [skip] NDI runtime + licenses file (STUB installer - built without the SDK)"
} else {
    Check "NDI runtime DLL installed beside the plugin" `
        (Test-Path (Join-Path $bundle "Contents\Win64\Processing.NDI.Lib.Advanced.x64.dll"))
    Check "NDI third-party licenses file installed beside the plugin" `
        (Test-Path (Join-Path $bundle "Contents\Win64\Processing.NDI.Lib.Licenses.txt"))
}

if (Test-Path $readme) {
    $text = Get-Content $readme -Raw
    Check "readme carries the NDI trademark line" `
        ($text -match "registered trademark of Vizrt NDI AB")
    Check "readme carries the ndi.video link" ($text -match "ndi\.video")
}

$arp = Get-ArpEntry
Check "Add-or-Remove-Programs entry registered" ($null -ne $arp)
if ($arp) {
    Check "ARP DisplayVersion is $version (got $($arp.DisplayVersion))" `
        ($arp.DisplayVersion -eq $version)
    Check "ARP entry has an UninstallString" (-not [string]::IsNullOrWhiteSpace($arp.UninstallString))
}

# --------------------------------------------------------------- uninstall ----
if ($arp -and $arp.UninstallString) {
    $uninst = $arp.UninstallString.Trim('"')
    Write-Host "Silent uninstall: $uninst"
    # Inno's uninstaller re-executes itself from %TEMP%, so -Wait returns before
    # the work is done - poll for the payload to disappear instead.
    Start-Process -FilePath $uninst -Wait `
        -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" | Out-Null
    $deadline = (Get-Date).AddSeconds(90)
    while ((Test-Path $bundle) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    Check "uninstall removes the bundle tree" (-not (Test-Path $bundle))
    # Its own budget: a slow payload removal must not starve this poll.
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-ArpEntry) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    Check "uninstall removes the ARP entry" ($null -eq (Get-ArpEntry))
} else {
    Check "uninstall could be driven from the ARP entry" $false
}

# ----------------------------------------------------------------- verdict ----
Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) check(s) failed:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    if (Test-Path $log) { Write-Host "Install log: $log" }
    exit 1
}
Write-Host "Installer acceptance checks passed (install -> verify -> uninstall -> clean)."
