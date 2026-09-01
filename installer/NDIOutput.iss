; Inno Setup script for the Windows plugin installer (ticket #23, spec
; decision 17). Compile it through scripts/package_windows_release.ps1 - that
; script is what stages the payload, enforces the NDI attribution files, and
; passes the version, so the installer can never carry a version the VERSION
; file disagrees with:
;
;   powershell -ExecutionPolicy Bypass -File .\scripts\package_windows_release.ps1
;
; Compiling this file directly needs the same defines:
;   ISCC.exe /DAppVersion=1.14.0 /DStageDir=..\stage /DOutputDir=..\dist installer\NDIOutput.iss
;
; v1 ships UNSIGNED with the SmartScreen click-through documented (spec
; decision 18). When Azure Artifact Signing arrives, sign both the payload and
; this installer via SignTool= in the compiler config - no script change here.

#ifndef AppVersion
  #error "AppVersion is not defined - build via scripts/package_windows_release.ps1"
#endif
#ifndef StageDir
  #define StageDir "..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist\v" + AppVersion
#endif
; Stub-linked builds (no NDI Advanced SDK, e.g. CI) get a name that cannot be
; mistaken for a release artifact - they compile and install but never stream.
#ifndef OutputBaseName
  #define OutputBaseName "NDIOutput-" + AppVersion + "-Windows-x64"
#endif

#define AppName "NDI Output for DaVinci Resolve"
#define BundleName "NDIOutput.ofx.bundle"
#define RepoUrl "https://github.com/lightsailvr/ResolveOFX_NDIOutput"

[Setup]
; Never change AppId - it is what makes a new version replace the old
; Add-or-Remove-Programs entry instead of stacking a second one.
AppId={{B7A6E2C4-5F31-4D8E-9A0B-3C6D1E4F7A82}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Light Sail VR
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases/latest
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}

; The OFX host only scans the Common Files plugin directory, so there is no
; install location to choose - the directory page is hidden, not merely
; prefilled, and UsePreviousAppDir=no keeps an old install from relocating it.
DefaultDirName={commoncf64}\OFX\Plugins\{#BundleName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
DirExistsWarning=no
UsePreviousAppDir=no
AllowNoIcons=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0

; The uninstaller lives outside the bundle: an OFX bundle should contain the
; plugin payload and nothing else. Inno removes this directory on uninstall.
UninstallFilesDir={commonpf64}\Light Sail VR\NDI Output
UninstallDisplayName={#AppName} {#AppVersion}

; Restart Manager would offer to close Resolve mid-edit; InitializeSetup below
; refuses the install instead, which is the honest behavior for a host app.
CloseApplications=no
RestartApplications=no

LicenseFile=..\LICENSE
InfoBeforeFile=README.txt
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Files]
; Whatever cmake --install staged, verbatim: the .ofx, the NDI runtime DLL and
; its licenses file, the Timeline (Auto) helper. ignoreversion because the
; payload must always be replaced - never version-compared (the macOS pkg
; learned this the hard way; see LEARNINGS 2026-08-30).
Source: "{#StageDir}\{#BundleName}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "README.txt"; DestDir: "{app}\Contents\Resources"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\Contents\Resources"; DestName: "LICENSE.txt"; Flags: ignoreversion

[InstallDelete]
; Replace the bundle tree wholesale so a file dropped in a later version never
; lingers from an earlier one.
Type: filesandordirs; Name: "{app}"

[Code]
function IsResolveRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := False;
  { find exits 0 only when the tasklist row is actually there }
  if Exec(ExpandConstant('{cmd}'),
          '/C tasklist /FI "IMAGENAME eq Resolve.exe" /NH | find /I "Resolve.exe" > nul',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := (ResultCode = 0);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if IsResolveRunning() then
  begin
    SuppressibleMsgBox('DaVinci Resolve is running.' + #13#10#13#10 +
      'Quit Resolve completely, then run this installer again - a loaded OpenFX' +
      ' plugin cannot be replaced, and Resolve only scans for plugins at startup.',
      mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if IsResolveRunning() then
  begin
    SuppressibleMsgBox('DaVinci Resolve is running.' + #13#10#13#10 +
      'Quit Resolve completely, then uninstall again - a loaded OpenFX plugin' +
      ' cannot be removed.',
      mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    Log('NDIOutput installed to ' + ExpandConstant('{app}'));
end;
