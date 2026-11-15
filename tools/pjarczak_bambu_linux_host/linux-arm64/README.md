# Running the Bambu Network plug-in on Linux arm64

Bambu Lab only publishes `libbambu_networking.so` for x86-64 Linux.  On an
arm64 machine (for example Fedora Asahi on Apple Silicon) the native arm64
build of OrcaSlicer therefore cannot load it, and the Device/Monitor tab and
cloud features stay unavailable.

This directory contains the runtime glue that runs the **unmodified x86-64
plug-in** inside a FEX/muvm microVM and bridges it to the **native arm64**
OrcaSlicer.  Slicing stays native and fast; only the network plug-in is
emulated.

## Architecture

```
 native arm64 OrcaSlicer
   │  dlopen("libbambu_networking.so")        <- the arm64 bridge module
   │
   ▼
 PJarczak Linux bridge (src/slic3r/Utils/PJarczakLinuxBridge)
   │  JSON frames over the wrapper's stdio
   ▼
 pjarczak_bambu_linux_host (this wrapper)
   │  socat TCP listener on 127.0.0.1
   ▼
 muvm microVM (FEX, x86-64 guest, Ubuntu 24.04 rootfs + WebKitGTK)
   │  bridge_relay.py connects back through passt's host-loopback mapping
   ▼
 pjarczak_bambu_linux_host_abi1 (x86-64)
   │  dlopen
   ▼
 libbambu_networking.so (x86-64, proprietary, from Bambu)
```

The guest cannot share pipes or FIFOs with the host, and vsock is not
available, so the transport is TCP through passt's host-loopback mapping
(the guest's default gateway is translated to the host's `127.0.0.1`).

## Requirements

- aarch64 Linux with FEX and muvm (`fex-emu`, `fex-emu-rootfs-fedora`,
  `muvm` on Fedora; `erofs-utils` for packing the rootfs)
- `socat`, `python3`, `podman` (for the amd64 builds)
- OrcaSlicer-bambulab built with the Linux bridge enabled
- the x86-64 plug-in payload (`libbambu_networking.so`, `libBambuSource.so`,
  `libagora_rtc_sdk.so`, `libagora-fdkaac.so`, `liblive555.so`)

The plug-in payload is not redistributable.  Let OrcaSlicer download it (it
fetches the x86-64 zip even on arm64; loading it natively fails, which is
expected), or copy it from the aarch64 BambuStudio Flatpak:
`~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/plugins/`.

## Build

1. **The app.**  Build OrcaSlicer normally (`./build_linux.sh -sr`).  The
   bridge is opt-in at runtime:

   ```sh
   PJARCZAK_LINUX_BRIDGE_ENABLED=1 \
   PJARCZAK_LINUX_BRIDGE_USE_NETWORK_MODULE=1 \
   ./build/src/Release/orca-slicer
   ```

   The executable must export its dynamic symbols (`ENABLE_EXPORTS`); the
   bridge module references `Slic3r::BBLModelTask` and friends from the
   executable.

2. **The x86-64 host.**

   ```sh
   ./build-host.sh
   ```

   This builds `pjarczak_bambu_linux_host_abi1` and `_abi0` in an amd64
   podman container.  Copy both into `~/.config/OrcaSlicer/plugins/`.

3. **The FEX rootfs.**

   ```sh
   ./build-rootfs.sh
   ```

   This extracts the official FEX Ubuntu 24.04 image, installs WebKitGTK 4.1
   into it (needed by the plug-in's login webview), and packs it to
   `~/.local/opt/orcaslicer-fex/ubuntu.erofs`.

4. **Install the wrapper.**

   ```sh
   ./install.sh
   ```

   Then install the arm64 bridge module as
   `~/.config/OrcaSlicer/plugins/libbambu_networking.so` (build target
   `pjarczak_bambu_networking_bridge`), and move the x86-64 payload into
   `~/.config/OrcaSlicer/plugins/runtime/`.

## Run

```sh
PJARCZAK_LINUX_BRIDGE_ENABLED=1 \
PJARCZAK_LINUX_BRIDGE_USE_NETWORK_MODULE=1 \
orca-slicer
```

The first network call after startup takes a few seconds while the microVM
boots.  Log in once; the token is stored in
`~/.config/OrcaSlicer/BambuNetworkEngine.conf`.

## Troubleshooting

- **"The URL can't be shown" in the login webview.**  `ZUserLogin::run()`
  starts an 8 s offline timer; under FEX the page can take longer.  The
  upstream fix is to raise that timeout.  (The prebuilt AppImage needed a
  binary patch; a source build only needs the constant changed.)
- **"Error resolving bambulab.com".**  WebKit's bubblewrap sandbox breaks DNS
  inside the microVM.  Set
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`.
- **Plug-in loads but every cloud call is empty.**  The plug-in's libcurl has
  no CA bundle in the guest.  The wrapper passes `SSL_CERT_FILE`,
  `CURL_CA_BUNDLE` and `SSL_CERT_DIR` into muvm; make sure they point at a
  readable bundle.
- **"Bridge module loaded, but the linux payload handshake did not return a
  version".**  A second instance is fighting over muvm's single-instance
  lock.  Run one instance at a time; kill leftovers:

  ```sh
  pkill -x orcaslicer_main; pkill -x muvm; pkill -x 'VM:fedora-asahi'
  ```

- **`undefined symbol: _ZN6Slic3r12BBLModelTaskC1Ev`.**  The executable was
  built without `ENABLE_EXPORTS`; the bridge module needs the app's dynamic
  symbols.
- **Host log.**  `/tmp/pjarczak-bridge-host.log` contains the relay and host
  output.

## Licensing

OrcaSlicer-bambulab is AGPL-3.0; these scripts are distributed under the same
license.  Bambu Lab's plug-in binaries are proprietary and are **not**
included here.
