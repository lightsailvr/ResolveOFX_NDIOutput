# Build the Windows release artifacts (ticket #23): the Inno Setup installer,
# a bare-bundle .zip for manual installs, and SHA-256 checksums - the Windows
# half of what scripts/package_release.sh produces on macOS. One VERSION, one
# CHANGELOG, one release event with per-platform artifacts.
#
#   cmake --install build --config Release --prefix stage
#   powershell -ExecutionPolicy Bypass -File .\scripts\package_windows_release.ps1
#
# Output lands in dist\v<VERSION>\:
#   NDIOutput-<VERSION>-Windows-x64.exe   installer (UNSIGNED - see README/BUILD.md)
#   NDIOutput-<VERSION>-Windows-x64.zip   bare bundle for manual installs
#   SHA256SUMS-Windows.txt
#
# Copy those three onto the macOS release machine's dist\v<VERSION>\ before
# running scripts/publish_github_release.sh, which attaches them next to the pkg.
#
# -AllowStub packages a stub-linked build (no NDI Advanced SDK on the machine,
# which is CI's situation). Such a build loads but never streams, so its
# artifacts are named ...-STUB.exe/.zip and must never be released.

param(
    [string]$Stage = "",
    [string]$OutDir = "",
    [switch]$AllowStub
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo "VERSION") -Raw).Trim()
if ($Stage -eq "") { $Stage = Join-Path $repo "stage" }
if ($OutDir -eq "") { $OutDir = Join-Path $repo "dist\v$version" }

$bundleName = "NDIOutput.ofx.bundle"
$bundle = Join-Path $Stage $bundleName
$binDir = Join-Path $bundle "Contents\Win64"
$resDir = Join-Path $bundle "Contents\Resources"

# ------------------------------------------------------------- preflight ----
$fail = $false
function Note($m) { Write-Host "  [ok] $m" }
function Miss($m) { Write-Host "  [MISSING] $m"; $script:fail = $true }

Write-Host "Preflight for v${version}:"

# CMake enforces this at configure time, but the stage tree may predate an edit.
$srcLine = Select-String -Path (Join-Path $repo "src\NDIOutputPlugin.cpp") `
    -Pattern 'kPluginVersionString\s+"([0-9]+\.[0-9]+\.[0-9]+)"' | Select-Object -First 1
$srcVer = if ($srcLine) { $srcLine.Matches[0].Groups[1].Value } else { "" }
if ($srcVer -eq $version) { Note "VERSION file matches source defines ($version)" }
else { Miss "VERSION ($version) != src define ('$srcVer') - run ./scripts/set_version.sh" }

if (Test-Path (Join-Path $binDir "NDIOutput.ofx")) { Note "staged plugin binary" }
else { Miss "$binDir\NDIOutput.ofx - run: cmake --install build --config Release --prefix $Stage" }

if (Test-Path (Join-Path $resDir "ndi_timeline_watch.py")) { Note "Timeline (Auto) helper staged" }
else { Miss "$resDir\ndi_timeline_watch.py" }

# NDI redistribution obligations (spec decision 13): the runtime rides inside
# the bundle and its third-party notices ride beside it.
$isStub = $false
$ndiDll = Join-Path $binDir "Processing.NDI.Lib.Advanced.x64.dll"
$ndiLic = Join-Path $binDir "Processing.NDI.Lib.Licenses.txt"
if ((Test-Path $ndiDll) -and (Test-Path $ndiLic)) {
    Note "NDI runtime DLL + third-party licenses file bundled"
} elseif ($AllowStub) {
    $isStub = $true
    Write-Host "  [warn] no NDI runtime in the stage tree - packaging a STUB build (-AllowStub)."
    Write-Host "         It installs and loads but does NOT stream. Never release it."
} else {
    Miss "$ndiDll and/or $ndiLic - build against the real NDI Advanced SDK (or pass -AllowStub)"
}

$iss = Join-Path $repo "installer\NDIOutput.iss"
if (Test-Path $iss) { Note "installer script" } else { Miss $iss }

# Inno Setup 6: PATH, then the default install locations, then the registry
# key its own installer writes (spec decision 17; preinstalled on the CI image).
# Per-user installs are covered too - Inno's own setup takes /CURRENTUSER, which
# is how you get a compiler onto a machine where you have no admin rights.
$iscc = ""
$cmd = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
if ($cmd) { $iscc = $cmd.Source }
if ($iscc -eq "") {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    foreach ($key in @(
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1",
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1",
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1")) {
        try {
            $loc = (Get-ItemProperty $key -ErrorAction Stop).InstallLocation
            if ($loc) { $candidates += (Join-Path $loc "ISCC.exe") }
        } catch {}
    }
    foreach ($c in $candidates) { if (Test-Path $c) { $iscc = $c; break } }
}
if ($iscc -ne "") { Note "Inno Setup compiler: $iscc" }
else { Miss "ISCC.exe (Inno Setup 6) - install from https://jrsoftware.org/isdl.php" }

if ($fail) {
    Write-Host ""
    Write-Error "Preflight failed - fix the [MISSING] items above (BUILD.md, Windows section)."
    exit 1
}

# ------------------------------------------------------------------ build ----
$suffix = if ($isStub) { "-STUB" } else { "" }
$baseName = "NDIOutput-$version-Windows-x64$suffix"

New-Item -ItemType Directory -Force $OutDir | Out-Null
$exeOut = Join-Path $OutDir "$baseName.exe"
$zipOut = Join-Path $OutDir "$baseName.zip"
Remove-Item -Force $exeOut, $zipOut -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Compiling installer..."
& $iscc "/Q" "/DAppVersion=$version" "/DStageDir=$((Resolve-Path $Stage).Path)" `
    "/DOutputDir=$((Resolve-Path $OutDir).Path)" "/DOutputBaseName=$baseName" $iss
if ($LASTEXITCODE -ne 0) { Write-Error "ISCC failed ($LASTEXITCODE)"; exit 1 }
if (-not (Test-Path $exeOut)) { Write-Error "ISCC reported success but $exeOut is missing"; exit 1 }
Write-Host "  built: $exeOut"

# The .zip is a manual-install path, so it carries the same attribution files
# the installer adds to the bundle - never just the raw stage tree.
$zipStage = Join-Path $OutDir "zipstage"
Remove-Item -Recurse -Force $zipStage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $zipStage | Out-Null
Copy-Item -Recurse $bundle (Join-Path $zipStage $bundleName)
$zipRes = Join-Path $zipStage "$bundleName\Contents\Resources"
Copy-Item (Join-Path $repo "installer\README.txt") $zipRes
Copy-Item (Join-Path $repo "LICENSE") (Join-Path $zipRes "LICENSE.txt")
Compress-Archive -Path (Join-Path $zipStage $bundleName) -DestinationPath $zipOut
Remove-Item -Recurse -Force $zipStage
Write-Host "  zipped bundle: $zipOut"

# --------------------------------------------------------------- checksums ----
$sums = Join-Path (Resolve-Path $OutDir).Path "SHA256SUMS-Windows.txt"
$lines = foreach ($f in @($exeOut, $zipOut)) {
    "{0}  {1}" -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(), (Split-Path -Leaf $f)
}
# Written by hand rather than Set-Content: this file is consumed by `sha256sum -c`
# on other platforms, which chokes on both a BOM and the CRLF line endings
# PowerShell would otherwise write (the macOS SHA256SUMS.txt is plain LF ASCII).
[System.IO.File]::WriteAllText($sums, ($lines -join "`n") + "`n",
    (New-Object System.Text.ASCIIEncoding))
Write-Host "  checksums: $sums"

Write-Host ""
if ($isStub) {
    Write-Host "STUB artifacts (pipeline proof only - they do not stream):"
} else {
    Write-Host "Release artifacts in ${OutDir}:"
}
Get-ChildItem $OutDir | Where-Object { -not $_.PSIsContainer } |
    Select-Object Name, Length | Format-Table -AutoSize
Write-Host "The installer is UNSIGNED: SmartScreen shows 'More info -> Run anyway'"
Write-Host "(documented in README.md and the release notes; spec decision 18)."
