<div align="center">

<picture>
  <img alt="OrcaSlicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

# OrcaSlicer-bambulab

**OrcaSlicer with full BambuNetwork cloud printing restored for Bambu Lab printers.**

Print over the internet from OrcaSlicer, the way it worked before — start,
monitor, and manage prints remotely through BambuNetwork instead of being
limited to LAN-only mode.

[Download the latest release](https://github.com/FULU-Foundation/OrcaSlicer-bambulab/releases) · [Report a bug](https://github.com/FULU-Foundation/OrcaSlicer-bambulab/issues) · [Security](SECURITY.md)

</div>

## What this is

OrcaSlicer is a free, open-source slicer for 3D printers. In January 2025,
Bambu Lab introduced its Authorization Control System (ACS) in printer
firmware. After that update, third-party slicers such as OrcaSlicer could no
longer send and control prints through Bambu's cloud (BambuNetwork) in the
normal way — full control was pushed to Bambu's own software, or to LAN-only
mode with Developer Mode enabled.

This fork of OrcaSlicer restores the BambuNetwork path:

- You are **not limited to LAN only**.
- It works **over the internet**, through BambuNetwork, with the normal
  functionality for everyday use and printing: sending prints, monitoring
  progress, and managing your printer remotely.
- It is based on the current OrcaSlicer codebase, so you keep OrcaSlicer's
  interface, calibration tools, and profiles.

This project exists so that people who own a printer can keep using the
slicer they chose with the hardware they bought. It is maintained by the
FULU Foundation.

## Download

Get the current build from the **Releases** page:

**https://github.com/FULU-Foundation/OrcaSlicer-bambulab/releases**

The current release is **v1.0.0** (May 12, 2026).

Before running a download, verify it against the `SHA256SUMS` file attached
to the release — see [SECURITY.md](SECURITY.md) for the exact steps. If a
release has no `SHA256SUMS` file, treat that as a reason to wait and ask in
an issue rather than to run the file.

> Placeholder note for maintainers: when a release is sealed with
> `tools/release_gate.py`, attach the generated `SHA256SUMS` to the GitHub
> release alongside the artifacts, and link the per-platform files here
> (Windows, Linux, macOS when available).

## Compatibility

Cloud (BambuNetwork) printing depends on your printer model, its firmware
version, and Bambu's Authorization Control System. Support is therefore
stated honestly: **verify your exact model and firmware before relying on
cloud printing**, and report what worked (or did not) in an issue with your
model and firmware version included.

| Printer | Expected support | Notes |
| --- | --- | --- |
| X1 / X1 Carbon | Supported — verify per firmware | ACS arrived on the X1 series first (early 2025). Cloud behavior differs between pre-ACS and ACS firmware; check your firmware version. |
| P1P / P1S | Supported — verify per firmware | ACS reached the P1 series in mid-2025 firmware. Behavior on the newest firmware should be confirmed per version. |
| A1 / A1 mini | Supported — verify per firmware | ACS reached the A1 series in mid-2025 firmware. Confirm your firmware version before reporting cloud issues. |
| H2 series (H2D / H2S / H2C) | Verify per model | H2-series printers shipped with ACS from day one, so there is no pre-ACS firmware baseline. Treat cloud support as to-be-confirmed per model and firmware until users report results. |
| Other Bambu Lab models | Not stated | If your model is not listed, open an issue with the model and firmware version rather than assuming support. |

LAN-only mode and Developer Mode remain available on supported firmware
independently of this fork — this fork is about restoring the cloud path,
not removing the LAN path.

## Installation

### Windows

Windows currently requires **WSL 2** (Windows Subsystem for Linux), because
the Bambu networking component runs through a Linux host bridge.

Before first launch, open Command Prompt or PowerShell **as Administrator**
and run:

```bat
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```

Restart Windows, then launch OrcaSlicer-bambulab.

We know this is friction, and a native Windows path that does not need
WSL 2 is the single biggest installation improvement on the roadmap. Until
then, the steps above are the honest requirement — if setup fails, file an
issue with your Windows version and the step that failed.

### Linux

On Linux, a normal installation is enough: download the Linux build from
the Releases page, make it executable if needed, and run it. No WSL or
bridge setup is required — the Linux host is native on this platform.

### macOS

**Work in progress.** There is no supported macOS build yet. This README
will be updated, and a release artifact added, when a macOS build is ready
to test. Please do not download macOS builds from unofficial mirrors.

## LAN vs Cloud — what is the difference?

- **LAN-only mode:** your computer talks directly to the printer on your
  local network. Nothing goes through Bambu's servers. It works at home on
  the same network, and on current firmware full control in LAN mode uses
  Developer Mode. It cannot start or monitor a print from outside your
  network.
- **Cloud (BambuNetwork) mode:** your slicer sends the job through Bambu's
  BambuNetwork service to your printer, the way Bambu Handy and Bambu
  Studio do. This is what lets you start and watch a print from anywhere
  with an internet connection. It requires signing in with your Bambu Lab
  account.
- **What this fork changes:** stock OrcaSlicer lost the normal cloud path
  after Bambu's authorization changes landed. This fork restores it, so
  OrcaSlicer users can again choose cloud printing instead of being
  confined to LAN-only.

Neither mode is "more correct" — LAN is more private and local, cloud is
more convenient. This fork is about keeping the choice with the printer's
owner.

## BMCU

We also encourage you to use BMCU, an open multi-color filament project
that works alongside Bambu printers. BMCU firmware is available in the
project maintainer's repositories.

## FAQ

**Is this Bambu Studio?**
No. This is OrcaSlicer — an independent, open-source slicer — with its
BambuNetwork support restored. It is not made by Bambu Lab.

**Do I need a Bambu Lab account?**
For cloud printing, yes — BambuNetwork is Bambu's service and signs in with
your Bambu Lab account, just like Bambu Handy does. LAN-only printing does
not need the cloud path.

**Will this work after I update my printer's firmware?**
It depends on the model and the firmware version — see the compatibility
table above. Firmware is the moving part in this project, which is why bug
reports must include your exact model and firmware version.

**Is it safe to log in?**
Read [SECURITY.md](SECURITY.md). In short: verify your download with
SHA256 checksums, only download from this repository's Releases page, and
report anything suspicious through the security process described there.

**Why does Windows need WSL 2?**
The restored networking path runs through a Linux host component. On Linux
that component is native; on Windows it currently runs inside WSL 2.
Removing that requirement is a known goal, not a hidden one.

**My printer crashed the app / the Device tab closed the program — what do I do?**
File a bug report with your printer model, firmware version, operating
system, and the steps that led to the crash. Known problems are tracked in
the issue tracker, and reports with logs attached are what get them fixed.

**Can I contribute?**
Yes. Bug reports with complete details, tested fixes, documentation
improvements, and compatibility reports (model + firmware + what worked)
are all useful. Keep changes focused, and describe how you tested them.

## License

OrcaSlicer is licensed under the **AGPL-3.0** license, and this fork is
distributed under the same license. OrcaSlicer is a fork of Bambu Studio,
which is a fork of PrusaSlicer — the AGPL-3.0 license is what guarantees
your right to use, study, modify, and share this software, and it applies
to this fork in full. See the `LICENSE` file in this repository for the
complete text.

The BambuNetwork networking component provided by Bambu Lab is Bambu Lab's
own software and remains subject to Bambu Lab's terms; this fork does not
relicense it.
