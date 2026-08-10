# OneKVM VNC 扩展

[English](README.md) | 简体中文

使用标准 VNC 客户端访问 OneKVM 所连接的计算机。这个独立进程的协议扩展
通过 Tight JPEG 路径转发硬件 MJPEG 帧，并通过 OneKVM 已鉴权的扩展控制
Socket 发送键盘和鼠标输入。

> **构建说明：** 本仓库只包含扩展源码。正式软件包由 `onekvm-distro` 中的
> OpenEmbedded（OE）配方统一构建；本仓库不提供独立的正式构建流程。

## 功能

- 使用 OneKVM 硬件 MJPEG 帧提供 Tight JPEG 视频
- VNC 运行时不解码或重新编码视频
- 支持键盘、绝对指针和鼠标扩展按键输入
- 可配置监听地址、TCP 端口、帧率和 JPEG 质量
- 支持可选的传统 VNC 密码鉴权

## 安装与启用

软件包名称是 `onekvm-extension-vnc`。请通过 OneKVM 发行版的软件包或镜像
安装，然后使用插件管理器控制扩展：

```sh
onekvm-plugin-manager enable vnc
onekvm-plugin-manager disable vnc
```

## 配置与连接

打开 **VNC** 扩展设置。服务默认监听 `0.0.0.0:5900`，通常可以使用以下
地址连接：

```text
<你的-onekvm-地址>:5900
```

帧率范围是 1 到 60 FPS，JPEG 质量范围是 1 到 100。传统 VNC 鉴权最多
接受 8 个字符的密码。由于这种鉴权强度较低且不会加密会话，请只在可信网络
或 VPN 中开放服务。

## 实现说明

C++20 运行时使用 LibVNCServer 中经 OpenBMC 验证的 Tight JPEG 路径，
直接把 OneKVM 的 MJPEG 帧传给 VNC 客户端，不进行解码或重新编码；客户端
输入则映射为已鉴权的 OneKVM HID 操作。扩展生命周期钩子独立于 Core 启动
和停止服务。

## 开发

使用 CMake 构建可选的键位映射测试：

```sh
cmake -S . -B build -DONEKVM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

[`NOTICE.openbmc`](NOTICE.openbmc) 记录了使用的上游版本和具体改动范围；
图标来源见 [`NOTICE.icons`](NOTICE.icons)。

## 许可证

本项目使用 GNU General Public License v2.0，完整条款见
[`LICENSE`](LICENSE)。打包或改编的上游组件继续使用其声明文件和许可证
文件中标明的条款。
