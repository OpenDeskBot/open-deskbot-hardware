# Open Deskbot

中文 | [English](README.md)

**Open Deskbot** 是一款开源桌面机器人：ESP32-S3 主控负责 OLED 表情、麦克风、扬声器与双轴云台；可选摄像头模块负责视觉上行。语音与视觉能力在**自建服务端**运行，固件仅通过 WiFi/WebSocket 对接，无厂商云绑定。

> 语音/大模型/TTS/视觉路由后端：请单独部署 [opendesk-service](https://github.com/OpenDeskBot/open-deskbot-service)。

## 项目愿景

- **硬件与固件开放** — 可自行组装、烧录与二次开发。
- **智能能力自建** — 音频与视频发往何处由你决定。
- **模块化** — 主控 ROM（`deskbot-rom`）与摄像头（`deskbot-camera`）为同一仓库内两个独立 PlatformIO 工程。

## 安装

**环境：** USB 线、Linux `dialout` 权限（或等价串口权限）、[PlatformIO](https://platformio.org/)（`pip install platformio`）。

```bash
git clone https://github.com/OpenDeskBot/open-deskbot-hardware.git
cd open-deskbot-hardware
```

烧录前配置 **`deskbot.local.env`**（主控与摄像头共用）：

```bash
cp deskbot.local.env.example deskbot.local.env
# 首次烧录会提示输入缺失的 WiFi（直接回车可跳过）
# DEVICE_ID 自动生成；服务器默认 39.107.38.241:9000
```

`flash_*.sh` 自动写入两个模块的固件。

### 主控（必选）

详见 **[deskbot-rom/README_zh.md](deskbot-rom/README_zh.md)**：

```bash
cd deskbot-rom
./flash_rom.sh all
```

### 摄像头模块（可选）

详见 **[deskbot-camera/README_zh.md](deskbot-camera/README_zh.md)**：

```bash
cd deskbot-camera
./flash_camera.sh all
```

### 后端服务

在可访问的主机上部署 **opendesk-service**，将上述 `platformio.local.ini` 中的 IP 指向该服务即可。

## 目录结构

```
open-deskbot-hardware/
├── deskbot-rom/       # ESP32-S3 主控固件 + flash_rom.sh
├── deskbot-camera/    # XIAO ESP32S3 Sense 摄像头 + flash_camera.sh
└── docs/              # 架构与贡献说明
```

| 主题 | 文档 |
|------|------|
| 系统架构 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 主控用法与参数 | [deskbot-rom/README_zh.md](deskbot-rom/README_zh.md) |
| 摄像头用法与参数 | [deskbot-camera/README_zh.md](deskbot-camera/README_zh.md) |
| 参与贡献 | [CONTRIBUTING.md](CONTRIBUTING.md) |

## 许可证

[GPLv3](LICENSE)。

## 作者

马克叔叔（Mark Yang）— mark.yang@ewen.ltd
