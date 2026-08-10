# OneKVM VNC 扩展

[English](README.md) | 简体中文

> **构建说明：** 本仓库只包含扩展源码。扩展统一由 `onekvm-distro` 中的
> OpenEmbedded（OE）配方构建和打包；本仓库不提供独立的正式构建流程。

这是 OneKVM 的进程外 VNC 协议支持。扩展独立维护运行时、manifest、
设置页面、测试及发布版本。

运行时使用 C++20 和 LibVNCServer 中经 OpenBMC 验证的 Tight JPEG 路径，
直接转发 OneKVM 的硬件 MJPEG 帧，不进行解码或重新编码；键盘和鼠标报告
则通过已鉴权的 OneKVM 扩展控制 Socket 发送。监听地址、端口、帧率、JPEG
质量和可选密码均可在扩展设置中配置。

`NOTICE.openbmc` 记录了使用的上游版本和具体改动范围。
