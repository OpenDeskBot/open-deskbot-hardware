# Contributing to Open Deskbot

Thank you for helping make this project ready for the open-source community.

## Before you start

1. Read [README.md](README.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
2. Do **not** commit virtualenvs (`.venv/`), `__pycache__/`, `.pio/`, logs, or audio/debug JSON artifacts.
3. Do **not** commit private hostnames, API keys, or internal URLs — use `platformio.local.ini` (gitignored) for firmware endpoints.

## Development setup

### Firmware

```bash
pip install platformio   # or: python3 -m venv .venv && .venv/bin/pip install platformio
cd deskbot-rom
cp platformio.local.ini.example platformio.local.ini   # optional: ASR host
pio run -e esp32s3_v2_0_1
```

Flash from module directory:

```bash
cd deskbot-rom && ./flash_rom.sh all [serial_port]
cd deskbot-camera && ./flash_camera.sh all [serial_port]
```

（`./flash_rom.sh` / `./flash_camera.sh` 不带参数可查看帮助与可用串口）

Device Web debugging and voice pipeline: deploy **opendesk-service** (`./start.sh` in that repo).

## Pull requests

1. Fork and create a feature branch from `main` / `master`.
2. Keep changes focused; match existing naming and comment style in touched files.
3. Update docs when you change user-visible behavior (README, `docs/`, or `deskbot-rom/doc/`).
4. Ensure CI passes (firmware build at minimum).
5. Write a clear PR description: **what**, **why**, and how you tested.

## Commit messages

Use imperative, concise subjects (English or 中文 is fine):

- `fix: skip ASR connect when host unset`
- `docs: update architecture diagram`

## Reporting issues

Include: board variant, firmware target (`deskbot-rom`), PlatformIO version, serial log excerpt, and steps to reproduce.

Security issues: see [SECURITY.md](SECURITY.md).

## License

By contributing, you agree that your contributions are licensed under the same [GPLv3](LICENSE) as the project.
