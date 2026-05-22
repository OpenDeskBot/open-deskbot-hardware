#!/usr/bin/env bash
# Deskbot ROM 固件工具：build | upload | log | all
set -euo pipefail

MODULE_ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$MODULE_ROOT/.." && pwd)"
FW_DIR="${OPEN_DESK_FW_DIR:-$MODULE_ROOT}"
PIO_ENV="${OPEN_DESK_PIO_ENV:-esp32s3_v2_0_1}"
LOG_FILE="${OPEN_DESK_APP_LOG:-$MODULE_ROOT/app.log}"
DEFAULT_PORT="${SERIAL_PORT:-/dev/ttyUSB0}"
SERIAL_PORTS=()

collect_serial_ports() {
  SERIAL_PORTS=()
  local p
  for p in /dev/ttyUSB* /dev/ttyACM*; do
    [[ -e "$p" ]] || continue
    SERIAL_PORTS+=("$p")
  done
}

first_serial_port() {
  if [[ ${#SERIAL_PORTS[@]} -gt 0 ]]; then
    printf '%s' "${SERIAL_PORTS[0]}"
  else
    printf '%s' "$DEFAULT_PORT"
  fi
}

list_serial_ports() {
  if [[ ${#SERIAL_PORTS[@]} -eq 0 ]]; then
    echo "  （未检测到 /dev/ttyUSB* /dev/ttyACM*，请插 USB 或检查 dialout 权限）"
    return
  fi
  local p
  for p in "${SERIAL_PORTS[@]}"; do
    echo "  $p"
  done
}

usage_copy_lines() {
  local port
  port="$(first_serial_port)"
  cat <<EOF
./flash_rom.sh build
./flash_rom.sh upload ${port}
./flash_rom.sh log ${port}
./flash_rom.sh all ${port}
EOF
  if [[ ${#SERIAL_PORTS[@]} -gt 1 ]]; then
    local p
    for p in "${SERIAL_PORTS[@]:1}"; do
      echo "./flash_rom.sh all ${p}"
    done
  fi
}

usage() {
  collect_serial_ports
  local port
  port="$(first_serial_port)"
  cat <<EOF
用法: ./flash_rom.sh build | upload | log | all [串口]

命令:
  build     仅编译
  upload    编译并烧录
  log       串口监视（Ctrl+C 结束，写入 ${LOG_FILE}）
  all       烧录 + 监视（改固件后常用）

当前串口（/dev/ttyUSB* /dev/ttyACM*）:
$(list_serial_ports)

复制即用（默认串口 ${port}）:
$(usage_copy_lines)

其它串口: 把上面命令里的 ${port} 换成列表中的设备名即可。

环境变量（可选）:
  SERIAL_PORT=/dev/ttyUSB0 ./flash_rom.sh all

device_id / 远程服务器 / WiFi（主控与摄像头共用）:
  仓库根目录 deskbot.local.env（不存在时从 deskbot.local.env.example 复制；已存在则不修改）
  详见 deskbot.local.env.example
EOF
}

resolve_pio() {
  if [[ -n "${PIO:-}" ]]; then
    return 0
  fi
  if [[ -x "$REPO_ROOT/.venv/bin/pio" ]]; then
    PIO="$REPO_ROOT/.venv/bin/pio"
  elif [[ -x "$MODULE_ROOT/.venv/bin/pio" ]]; then
    PIO="$MODULE_ROOT/.venv/bin/pio"
  elif [[ -x "${HOME}/.local/bin/pio" ]]; then
    PIO="${HOME}/.local/bin/pio"
  elif command -v pio >/dev/null 2>&1; then
    PIO="$(command -v pio)"
  else
    echo "未找到 pio（pip install platformio）" >&2
    exit 1
  fi
}

prepare_flash_env() {
  # shellcheck source=../scripts/deskbot_flash_config.sh
  source "$REPO_ROOT/scripts/deskbot_flash_config.sh"
  prepare_deskbot_flash_config "$FW_DIR" "$PIO_ENV" "rom"
}

# 解析命令后的可选串口参数
pick_port() {
  if [[ "${1:-}" == /dev/* ]]; then
    printf '%s' "$1"
  else
    printf '%s' "$DEFAULT_PORT"
  fi
}

require_port() {
  local port=$1
  if [[ -e "$port" ]]; then
    return 0
  fi
  collect_serial_ports
  echo "串口不存在: $port" >&2
  echo "可用设备:" >&2
  list_serial_ports >&2
  echo >&2
  echo "复制即用:" >&2
  usage_copy_lines >&2
  exit 1
}

free_serial_port() {
  local dev=$1
  [[ -e "$dev" ]] || return 0

  if command -v lsof >/dev/null 2>&1; then
    local p
    for p in $(lsof -t "$dev" 2>/dev/null || true); do
      [[ -n "$p" ]] || continue
      kill -TERM "$p" 2>/dev/null || true
    done
    sleep 0.25
    for p in $(lsof -t "$dev" 2>/dev/null || true); do
      [[ -n "$p" ]] || continue
      kill -KILL "$p" 2>/dev/null || true
    done
  elif command -v fuser >/dev/null 2>&1; then
    fuser -k -TERM "$dev" 2>/dev/null || true
    sleep 0.2
  fi

  pkill -TERM -f "pio.*device monitor.*${dev}" 2>/dev/null || true
  pkill -TERM -f "picocom.*${dev}" 2>/dev/null || true
  sleep 0.15
}

cmd_build() {
  prepare_flash_env
  echo "==> 编译 ($FW_DIR / $PIO_ENV)"
  (cd "$FW_DIR" && "$PIO" run -e "$PIO_ENV")
}

cmd_upload() {
  prepare_flash_env
  local port
  port="$(pick_port "${1:-}")"
  require_port "$port"
  echo "==> 释放串口: $port"
  free_serial_port "$port"
  echo "==> 烧录: $port"
  (cd "$FW_DIR" && "$PIO" run -e "$PIO_ENV" -t upload --upload-port "$port")
  echo "==> 完成"
}

cmd_log() {
  local port
  port="$(pick_port "${1:-}")"
  require_port "$port"
  echo "==> 释放串口: $port"
  free_serial_port "$port"
  echo "==> 监视 $port → $LOG_FILE （Ctrl+C 结束）"
  touch "$LOG_FILE"
  if command -v stdbuf >/dev/null 2>&1; then
    stdbuf -oL -eL "$PIO" device monitor -p "$port" -d "$FW_DIR" -e "$PIO_ENV" 2>&1 \
      | stdbuf -oL -eL tee -a "$LOG_FILE"
  else
    "$PIO" device monitor -p "$port" -d "$FW_DIR" -e "$PIO_ENV" 2>&1 | tee -a "$LOG_FILE"
  fi
}

cmd_all() {
  local port
  port="$(pick_port "${1:-}")"
  require_port "$port"
  cmd_upload "$port"
  echo "==> 等待复位…"
  sleep 1
  cmd_log "$port"
}

# --- main ---
if [[ $# -eq 0 ]] || [[ "${1:-}" == help ]] || [[ "${1:-}" == -h ]] || [[ "${1:-}" == --help ]]; then
  usage
  exit 0
fi

resolve_pio
CMD="$1"
shift || true

case "$CMD" in
  build)
    cmd_build
    ;;
  upload)
    cmd_upload "${1:-}"
    ;;
  log)
    cmd_log "${1:-}"
    ;;
  all)
    cmd_all "${1:-}"
    ;;
  *)
    echo "未知命令: $CMD" >&2
    echo >&2
    usage >&2
    exit 1
    ;;
esac
