# Firmware

ESP32-S3 production firmware for **Deskbot**, built with [PlatformIO](https://platformio.org/) (Arduino framework).

## Layout

```
deskbot-rom/
├── platformio.ini          # production env: esp32s3_v2_0_1
└── firmware/               # src_dir — all production sources
```

## Build & flash

From `deskbot-rom/` (recommended):

```bash
./flash_rom.sh upload [ /dev/ttyUSB0 ]
./flash_rom.sh all
```

From module root:

```bash
pio run -e esp32s3_v2_0_1
pio run -e esp32s3_v2_0_1 -t upload --upload-port /dev/ttyUSB0
```

## ASR / voice backend (required for chat)

Copy and edit local config (gitignored):

```bash
cp platformio.local.ini.example platformio.local.ini
```

Set `ASR_CHAT_HOST` and `ASR_CHAT_PORT` to your WebSocket `asr_chat` service.

## Default WiFi (first boot)

```bash
cp firmware/wifi_defaults.h.example firmware/wifi_defaults.h
```

Leave `WIFI_DEFAULT_SSID` empty to use captive portal only. `wifi_defaults.h` is gitignored.

## Environment variables (flash script)

| Variable | Default | Meaning |
|----------|---------|---------|
| `OPEN_DESK_FW_DIR` | `deskbot-rom/` | PlatformIO project path |
| `OPEN_DESK_PIO_ENV` | `esp32s3_v2_0_1` | PlatformIO environment name |
| `SERIAL_PORT` | `/dev/ttyUSB0` | USB serial device |

See [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for module map.
