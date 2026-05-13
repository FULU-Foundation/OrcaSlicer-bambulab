<p align="center">
  <img alt="OrcaSlicer logo" src="resources/images/OrcaSlicer.png" width="15%">
</p>

# OrcaSlicer — BambuNetwork edition

This version of OrcaSlicer restores full BambuNetwork support for Bambu Lab
printers. You are not limited to LAN only — it works over the internet just
like before, through BambuNetwork, with full functionality for normal use
and printing.

## Installation

### Windows

Windows requires WSL 2. Before the first launch, open Command Prompt or
PowerShell as Administrator and run:

```bat
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```

Restart Windows, then launch Orca Studio.

### Linux

A normal installation is enough.

### macOS

Supported on both Apple Silicon and Intel. On first launch OrcaSlicer
auto-installs a small Lima VM (`orcaslicer-bambu-network`) that runs the
Linux BambuNetwork plug-in under a native x86_64 Linux guest. You'll be
prompted to install the runtime when you launch the app for the first
time; accepting will:

- install Lima and `qemu-system-x86_64` via Homebrew if they're missing,
- bring up an Ubuntu x86_64 VM in the background,
- copy the BambuNetwork payload into the VM.

Once the runtime is ready, log in to Bambu Cloud normally — printers,
camera preview, and Send-to-Printer (cloud or LAN) all work as on other
platforms.

Notes:

- The first runtime install downloads an Ubuntu cloud image and takes a
  few minutes; subsequent launches are immediate.
- Building from source on macOS 15 (Tahoe) is supported via
  `./build_release_macos.sh`. Ninja is the default generator; `-x`
  switches to the Xcode generator if you have a full Xcode.app install.

## BMCU

The BMCU companion firmware is also encouraged. The firmware lives in a
separate repository in the upstream maintainer's account.
</content>
</invoke>