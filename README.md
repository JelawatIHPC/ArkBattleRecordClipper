# 明日方舟作战记录自动剪辑器 (ArkBattleRecordClipper)

<div align="center">

![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)
![C++20](https://img.shields.io/badge/Language-C++20-orange.svg)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

**一个堪称高效的明日方舟录屏剪辑工具**

[功能特性](#功能特性) • [快速开始](#快速开始) • [编译指南](#编译指南) • [使用说明](#使用说明) • [贡献指南](#贡献指南) • [常见问题](#常见问题)

</div>

## 项目概述

**ArkBattleRecordClipper** 是一个专为明日方舟蓟县玩家设计的视频剪辑工具。它能够自动识别并剪辑游戏录屏中的暂停片段，同时尽可能保留攻略者的操作，减少视频时长，避免视频发布后折磨~~实际上并不存在的~~观众。

- 基于 OpenCV 的图像识别技术，精准定位游戏 UI 元素
- 支持硬件加速编解码，处理速度提升显著
- 提供多种剪辑策略和加速选项，满足不同需求
- 现代化图形界面，操作简单直观

---

## 功能特性

- **多编码器支持**：自动检测并选择编码器，或手动指定，支持 NVENC、AMF、QSV 等硬件编码器
- **解码支持**：通过 DXVA2 实现硬件加速解码，或是使用兼容性更好的纯 CPU 解码
- **自定义码率**：自动使用源视频码率，或是手动指定输出视频码率
- **剪辑策略配置**：根据用户需求调节剪辑策略，如一倍速加速到二倍速、子弹时间加速指定倍数等
- **实时进度**：显示处理进度、FPS、队列深度、预计剩余时间

---

## 系统要求

- Windows 10 x64 或更高版本（推荐 Windows 11 x64）
- Webview 支持（Windows 10 1803+）

---

## 快速开始

1. 从 [Releases](https://github.com/your-repo/releases) 页面下载最新版本，解压到任意目录
2. 双击运行 `ArkBattleRecordClipper.exe`
3. 选择视频文件，配置参数，点击"启动"

---

## 编译指南

### 从源代码编译

#### 1. 安装 Xmake

Xmake 是项目使用的构建和包管理工具。可访问 [Xmake 官网](https://xmake.io/#/zh-cn/guide/installation) 下载安装程序，或通过包管理器安装。

#### 2. 安装 Visual Studio 工具链

本项目使用 MSVC 编译器和 Windows SDK 开发，推荐使用相同工具链编译：

1. 安装 [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
2. 在安装程序中选择"使用 C++ 的桌面开发"工作负载
3. 确保安装了 Windows 10/11 SDK

#### 3. 标准编译流程

```bash
# 进入项目目录
cd ArkBattleRecordClipper

# 配置项目（可选：指定模式）
xmake f -m release

# 编译项目
xmake build

# 运行程序
xmake run
```

<div style="background-color: #e8f7f4; border: 1px solid #1abc9c; border-radius: 6px; padding: 16px; margin: 16px 0;">
  <div style="display: flex; align-items: center; margin-bottom: 12px;">
    <span style="font-size: 20px; margin-right: 8px;">💡</span>
    <strong style="color: #16a085; font-size: 16px;">Tip</strong>
  </div>
  <div style="color: #2c3e50; line-height: 1.6;">
    <p>
Xmake 会自动从 Github 等源拉取如下项目依赖：

- [FFmpeg (BtbN LGPL Release)](https://github.com/BtbN/FFmpeg-Builds/releases)
- [OpenCV](https://github.com/opencv/opencv/)
- [webview](https://github.com/webview/webview)
- [nlohmann/json](https://github.com/nlohmann/json)
    </p>
  </div>
</div>

### 打包发布

#### 打包可执行文件

```bash
# 安装到指定目录
xmake install -o ./output

# 输出目录结构：
# output/
# ├── ArkBattleRecordClipper.exe
# ├── assets/
# │   ├── ...
# └── *.dll (FFmpeg, OpenCV 动态库)
```

## 使用说明

### 启动界面

点击"选择文件"按钮，在文件对话框中选择录屏视频文件，支持 MP4、AVI、MKV、MOV、WMV、FLV、WebM 等格式。输出文件名会自动生成 `_clipped` 后缀，可手动修改，输出目录为 exe 同一目录。输出视频码率可调（Auto 表示使用源视频码率）。

### 策略配置

#### 一倍速加速

- 开启后，无操作的一倍速片段会被自动加速至二倍速

#### 子弹时间加速

- 调节范围：1.0x - 5.0x
- 选中干员的片段会被加速至指定倍数

#### 选中动画保留

- 调节范围：0.0s - 1.0s，默认值：0.3s
- 在选中/取消选中干员时，强制保留指定长度的片段，避免剪掉视角切换动画，推荐至少设为 0.2s
- 若视频中存在连续操作片段，感受到视角颠簸，可尝试增大该值

### 硬件加速设置

#### 编码器选择

| 编码器 | 描述 | 推荐场景 |
|--------|------|----------|
| 自动 | 自动检测并选择最佳编码器 | 通用场景 |
| NVIDIA NVENC | NVIDIA 显卡硬件编码 | RTX 系列 GPU |
| AMD AMF | AMD 显卡硬件编码 | RX 系列 GPU |
| Intel QSV | Intel 核显硬件编码 | Intel 集成显卡 |
| CPU | 纯 CPU 编码 | 无独立显卡或兼容性问题 |

#### 解码器选择

| 解码器 | 描述 | 推荐场景 |
|--------|------|----------|
| DXVA2 | DirectX 视频加速 | 支持 GPU 解码的显卡 |
| CPU | 纯 CPU 解码 | 兼容性最佳 |

---

## 贡献指南

本项目尚需多种不同输入视频，来进一步测试算法是否完善，程序在不同平台的兼容性也未经广泛系统性测试。如果您发现本项目在处理某些视频时存在异常，可提交 Issue 并说明硬件平台，或联系作者直接提供视频。

### 联系方式

QQ: 420057679
Bilibili: [Jelawat地鼠的个人空间](https://space.bilibili.com/13913639)

---

## 常见问题

~~由于没人提 Issue 所以实际上并不常见~~

### 编译问题

**Q: Xmake 下载依赖失败怎么办？**

A: Xrepo 从 GitHub 下载依赖，请确保网络畅通。可以尝试：
- 参考 Xmake 官方文档加速 GitHub 连接，或自行配置代理
- 根据 Xmake 输出信息手动下载安装依赖包

## 许可证

本项目采用 MIT 许可证。详见 [LICENSE](LICENSE) 文件。

### 第三方库许可证

本项目使用以下开源库：

| 库 | 许可证 | 说明 |
|---|--------|------|
| FFmpeg | LGPL-3.0 | 视频编解码 |
| OpenCV | Apache-2.0 | 图像处理 |
| webview | MIT | WebView 封装 |
| nlohmann/json | MIT | JSON 解析 |

详见 [NOTICE](NOTICE) 文件。
