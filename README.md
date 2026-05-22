# Open Deskbot

[中文](README_zh.md) | English

**Open Deskbot** is an open-source desktop robot: a small ESP32-S3 body with OLED face, microphone, speaker, and a 2-DOF head, plus an optional camera module. Voice and vision run on **your own server**—firmware connects over WiFi/WebSocket; no cloud lock-in.

> Backend (ASR / LLM / TTS / vision routing): deploy [opendesk-service](https://github.com/OpenDeskBot/open-deskbot-service) separately.

## Vision

- **Open hardware & firmware** — build, flash, and extend the robot yourself.
- **Self-hosted intelligence** — you control where audio and video go.
- **Composable modules** — main ROM (`deskbot-rom`) and camera (`deskbot-camera`) are independent PlatformIO projects in one repo.

## Install

**Requirements:** USB cable, Linux `dialout` group (or equivalent serial access), [PlatformIO](https://platformio.org/) (`pip install platformio`).

```bash
git clone https://github.com/OpenDeskBot/open-deskbot-hardware.git
cd open-deskbot-hardware
```

Configure **`deskbot.local.env`** at repo root (shared by ROM + camera):

```bash
cp deskbot.local.env.example deskbot.local.env
# First flash prompts for missing WiFi (Enter to skip)
# DEVICE_ID auto-generated; server defaults 39.107.38.241:9000
```

Flash scripts inject WiFi + device_id + server into both modules.

### Main controller (required)

See **[deskbot-rom/README.md](deskbot-rom/README.md)**:

```bash
cd deskbot-rom
./flash_rom.sh all
```

### Camera module (optional)

See **[deskbot-camera/README.md](deskbot-camera/README.md)**:

```bash
cd deskbot-camera
./flash_camera.sh all
```

### Backend

Deploy **opendesk-service** on a reachable host. Use the same server IP in both `platformio.local.ini` files above.

## Repository layout

```
open-deskbot-hardware/
├── deskbot-rom/       # ESP32-S3 main firmware + flash_rom.sh
├── deskbot-camera/    # XIAO ESP32S3 Sense camera + flash_camera.sh
└── docs/              # Architecture, contributing
```

| Topic | Document |
|-------|----------|
| System architecture | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Main ROM (usage & specs) | [deskbot-rom/README.md](deskbot-rom/README.md) |
| Camera (usage & specs) | [deskbot-camera/README.md](deskbot-camera/README.md) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md) |

## License

[GNU General Public License v3.0](LICENSE).

## Author

Mark Yang — mark.yang@ewen.ltd
