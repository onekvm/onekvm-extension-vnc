# OneKVM VNC Extension

English | [简体中文](README.zh-CN.md)

Access the computer connected to OneKVM with a standard VNC client. This
out-of-process protocol extension forwards hardware MJPEG frames through the
Tight JPEG path and sends keyboard and pointer input through OneKVM's
authenticated extension control socket.

> **Build note:** This repository contains extension sources only. Release
> packages are built by the OpenEmbedded recipes in `onekvm-distro`; this
> repository does not provide a standalone release build.

## Features

- Tight JPEG clients (TigerVNC, noVNC, and similar) receive hardware MJPEG frames directly
- Other clients fall back to the RGB framebuffer with Raw, ZRLE, or Hextile and 32×32 dirty rectangles
- Tight JPEG is still a full frame; dirty rectangles apply only to the fallback path so clients without JPEG are not dropped
- Keyboard, absolute pointer, and additional mouse-button input
- Configurable bind address, TCP port, frame rate, and JPEG quality
- Optional classic VNC password authentication

## Install and enable

The package name is `onekvm-extension-vnc`. Install it through your OneKVM
distribution package or image, then manage it with the plugin manager:

```sh
onekvm-plugin-manager enable vnc
onekvm-plugin-manager disable vnc
```

## Configure and connect

Open the **VNC** extension settings. The server listens on `0.0.0.0:5900` by
default, so a client can normally connect to:

```text
<your-onekvm-host>:5900
```

Frame rate can be set from 1 to 60 FPS and JPEG quality from 1 to 100. Classic
VNC authentication accepts passwords of at most eight characters. Because that
authentication scheme is weak and does not encrypt the session, expose the
service only on a trusted network or through a VPN.

## How it works

The C++20 runtime uses LibVNCServer's OpenBMC-proven Tight JPEG path. It passes
OneKVM's MJPEG frames to VNC clients without decoding or re-encoding them, then
maps client input to authenticated OneKVM HID operations. Extension lifecycle
hooks start and stop the server independently of Core.

## Development

Build the optional keymap test with CMake:

```sh
cmake -S . -B build -DONEKVM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

See [`NOTICE.openbmc`](NOTICE.openbmc) for the exact upstream revision and
adaptation scope. Icon provenance is documented in
[`NOTICE.icons`](NOTICE.icons).

## License

This project is licensed under the GNU General Public License v2.0. See
[`LICENSE`](LICENSE). Bundled or adapted upstream components retain the license
terms identified by their notice and license files.
