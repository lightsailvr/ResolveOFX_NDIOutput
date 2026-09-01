NDI Output for DaVinci Resolve
==============================

An OpenFX plugin that streams the frame DaVinci Resolve renders as an NDI®
source on your local network — SDR and HDR, GPU-accelerated on NVIDIA hardware.

Installs to:
  C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle


After installing
----------------
1. Fully restart DaVinci Resolve — OpenFX plugins are only scanned at startup.
2. Color page -> OpenFX -> LSVR -> NDIOutput; drag it onto a clip.
3. Start playback and open the stream in any NDI receiver — for example Studio
   Monitor from the free NDI Tools (https://ndi.video/tools/).

The first send raises a Windows Firewall prompt for Resolve. Allow it on
private networks; declining leaves the source discoverable but its video
unreachable from other machines (the "listed but black" symptom).


Requirements
------------
DaVinci Resolve 17 or later (tested on 20) on 64-bit x64 Windows 10/11 -
Windows on ARM is not supported. Nothing else is required: the NDI runtime
ships inside the plugin bundle.


Unsigned installer / SmartScreen
--------------------------------
This build is not code-signed, so Windows SmartScreen shows "Windows protected
your PC" the first time you run it. Click "More info", then "Run anyway".
To check the download without relying on a signature, compare its SHA-256
against SHA256SUMS-Windows.txt published with the release:

  Get-FileHash .\NDIOutput-<version>-Windows-x64.exe -Algorithm SHA256


Silent install (fleet deployment)
---------------------------------
  NDIOutput-<version>-Windows-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART

Push it to machines with Resolve closed: with Resolve running the installer
exits non-zero and installs nothing rather than touching a loaded plugin.

Uninstall from "Add or Remove Programs", or run the registered uninstaller with
the same switches. DaVinci Resolve must be closed for both — a loaded plugin
cannot be replaced or removed, and the installer refuses to try.


Attribution and licensing
-------------------------
NDI® is a registered trademark of Vizrt NDI AB — https://ndi.video/

This product uses the NDI Advanced SDK. The SDK's third-party license notices
are installed beside the plugin binary as
Contents\Win64\Processing.NDI.Lib.Licenses.txt.

Plugin source, documentation and license:
https://github.com/lightsailvr/ResolveOFX_NDIOutput
