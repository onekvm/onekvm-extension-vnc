# OneKVM VNC Extension

English | [简体中文](README.zh-CN.md)

> **Build note:** This repository contains extension sources only. The
> extension is built and packaged by the OpenEmbedded recipes in
> `onekvm-distro`; this repository does not provide a standalone release build.

Out-of-process VNC support for OneKVM. The repository owns its runtime,
manifest, settings layout, tests, and release version independently from the
core OS repository.

The runtime is C++20 and uses LibVNCServer's OpenBMC-proven Tight JPEG path.
It forwards OneKVM's hardware MJPEG frames without decoding or re-encoding
them, and routes keyboard and pointer reports through the authenticated OneKVM
extension control socket.

See `NOTICE.openbmc` for the exact upstream revision and adaptation scope.
