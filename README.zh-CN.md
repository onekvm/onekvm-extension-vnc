# OneKVM VNC 扩展

[English](README.md) | 简体中文

使用标准 VNC 客户端访问 OneKVM 所连接的计算机。这个独立进程的协议扩展
通过 RFB 原生 JPEG 或 Tight JPEG 转发硬件 MJPEG 帧，并通过 OneKVM 已鉴权的扩展控制
Socket 发送键盘和鼠标输入。兼容的 Viewer 还能通过标准 QEMU RFB 音频扩展接收
HDMI 音频。

> **构建说明：** 本仓库只包含扩展源码。正式软件包由 `onekvm-distro` 中的
> OpenEmbedded（OE）配方统一构建；本仓库不提供独立的正式构建流程。

## 功能

- 支持 RFB 原生 JPEG（encoding 21）或 Tight JPEG 的客户端直接转发硬件 MJPEG
- 其它客户端回退到 RGB 帧缓冲，用 Raw / ZRLE / Hextile，并按 32×32 脏矩形更新
- 直接 JPEG 仍发整帧；脏矩形只作用在回退路径，避免把不支持 JPEG 的客户端踢掉
- 支持 QEMU RFB 音频（`-259`），按 Viewer 请求转换 PCM 格式、声道数和采样率
- 支持键盘、绝对指针和鼠标扩展按键输入
- 可配置监听地址、TCP 端口、帧率和 JPEG 质量
- 支持可选的传统 VNC 密码鉴权

## 安装与启用

软件包名称是 `onekvm-extension-vnc`。请通过 OneKVM 发行版的软件包或镜像
安装，然后使用插件管理器控制扩展：

```sh
onekvm-extension enable vnc
onekvm-extension disable vnc
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

C++20 运行时使用 LibVNCServer，并补充当前 RFB 注册表定义的原生 JPEG
encoding 21。协商了原生 JPEG 或 Tight JPEG 的客户端直接收硬件 MJPEG；
未声明 JPEG 的客户端才回退到 RGB 帧缓冲（Raw / ZRLE / Hextile，32×32
脏矩形）。客户端输入映射为已鉴权的 OneKVM HID 操作。只有 Viewer 声明并启用
QEMU 音频后才订阅共享 Opus 流，解码一次后按协商参数转换为 PCM；没有声明
QEMU 音频能力的 Viewer 保持纯视频连接。

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
