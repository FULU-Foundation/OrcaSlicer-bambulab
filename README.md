<div align="center">

<picture>
  <img alt="OrcaSlicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

## This version of OrcaSlicer restores full BambuNetwork support for Bambu Lab printers.

You are not limited to LAN only.  
It works over the internet just like before, through BambuNetwork, with full functionality for normal use and printing.

## Installation

### Windows

Windows requires WSL 2.

Before first launch, open Command Prompt or PowerShell as Administrator and run:

```bat
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```

Restart Windows, then launch Orca Studio.

### Linux

On Linux, a normal installation is enough.

### macOS

macOS uses [Lima](https://lima-vm.io/) (Linux VM) to run the BambuNetwork bridge.

Prerequisites (install with [Homebrew](https://brew.sh)):

```bash
brew install lima
```

On Apple Silicon (M1/M2/M3/M4), also install Rosetta if not already present:

```bash
softwareupdate --install-rosetta --agree-to-license
```

Then open Orca Studio normally — on first launch it bootstraps the Lima VM (`orcaslicer-bambu-network`) and copies the bridge runtime into `~/Library/Application Support/OrcaSlicer/macos-bridge/`. If Lima or Rosetta are missing, the bundled installer (`install_runtime_macos.sh` inside the `.app`) will try to fetch them automatically.

Minimum macOS: 11.3 (13+ recommended for vz/vzNAT acceleration).


## BMCU

I also encourage you to use BMCU.

You can find BMCU firmware in my repositories.

</div>
