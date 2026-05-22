#!/usr/bin/env bash
# Shared flash config for deskbot-rom + deskbot-camera (sourced by flash_*.sh).
# Root config: deskbot.local.env (gitignored).
# 不存在时从 deskbot.local.env.example 复制；已存在则只读取，不修改。

deskbot_flash_config_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKBOT_REPO_ROOT="$(cd "$deskbot_flash_config_dir/.." && pwd)"
DESKBOT_LOCAL_ENV="${DESKBOT_LOCAL_ENV:-$DESKBOT_REPO_ROOT/deskbot.local.env}"
DESKBOT_LOCAL_ENV_EXAMPLE="${DESKBOT_LOCAL_ENV_EXAMPLE:-$DESKBOT_REPO_ROOT/deskbot.local.env.example}"

DESKBOT_DEFAULT_WS_HOST="${DESKBOT_DEFAULT_WS_HOST:-39.107.38.241}"
DESKBOT_DEFAULT_WS_PORT="${DESKBOT_DEFAULT_WS_PORT:-9000}"

deskbot_parse_bool() {
  local val="${1:-}" default="${2:-0}"
  if [[ -z "$val" ]]; then
    printf '%s' "$default"
    return 0
  fi
  case "${val,,}" in
    1 | true | yes | on) printf '1' ;;
    0 | false | no | off) printf '0' ;;
    *) return 1 ;;
  esac
}

deskbot_escape_c_string() {
  local s=$1
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  printf '%s' "$s"
}

deskbot_env_has_key() {
  local key=$1
  [[ -f "$DESKBOT_LOCAL_ENV" ]] || return 1
  grep -qE "^[[:space:]]*${key}[[:space:]]*=" "$DESKBOT_LOCAL_ENV" 2>/dev/null
}

# 读取 KEY=value（允许等号两侧空格；去掉一层引号）
deskbot_env_get() {
  local key=$1
  local line val
  [[ -f "$DESKBOT_LOCAL_ENV" ]] || return 1
  line="$(grep -E "^[[:space:]]*${key}[[:space:]]*=" "$DESKBOT_LOCAL_ENV" 2>/dev/null | tail -1 || true)"
  [[ -n "$line" ]] || return 1
  val="${line#*=}"
  val="${val#"${val%%[![:space:]]*}"}"
  val="${val%"${val##*[![:space:]]}"}"
  if [[ "$val" =~ ^\"(.*)\"$ ]]; then
    val="${BASH_REMATCH[1]}"
  elif [[ "$val" =~ ^\'(.*)\'$ ]]; then
    val="${BASH_REMATCH[1]}"
  fi
  printf '%s' "$val"
}

deskbot_load_local_env() {
  DEVICE_ID=""
  DESKBOT_WS_HOST=""
  DESKBOT_WS_PORT=""
  WIFI_SSID=""
  WIFI_PASSWORD=""
  CAMERA_UPLOAD_JPEG=""
  CAMERA_LOCAL_FACE_DETECT=""
  CAMERA_UPLOAD_FPS=""
  CAMERA_FACE_DETECT_FPS=""
  [[ -f "$DESKBOT_LOCAL_ENV" ]] || return 0
  DEVICE_ID="$(deskbot_env_get DEVICE_ID 2>/dev/null || true)"
  DESKBOT_WS_HOST="$(deskbot_env_get DESKBOT_WS_HOST 2>/dev/null || true)"
  DESKBOT_WS_PORT="$(deskbot_env_get DESKBOT_WS_PORT 2>/dev/null || true)"
  WIFI_SSID="$(deskbot_env_get WIFI_SSID 2>/dev/null || true)"
  WIFI_PASSWORD="$(deskbot_env_get WIFI_PASSWORD 2>/dev/null || true)"
  CAMERA_UPLOAD_JPEG="$(deskbot_env_get CAMERA_UPLOAD_JPEG 2>/dev/null || true)"
  CAMERA_LOCAL_FACE_DETECT="$(deskbot_env_get CAMERA_LOCAL_FACE_DETECT 2>/dev/null || true)"
  CAMERA_UPLOAD_FPS="$(deskbot_env_get CAMERA_UPLOAD_FPS 2>/dev/null || true)"
  CAMERA_FACE_DETECT_FPS="$(deskbot_env_get CAMERA_FACE_DETECT_FPS 2>/dev/null || true)"
}

deskbot_validate_device_id() {
  local id=$1
  [[ "$id" =~ ^deskbot_[a-zA-Z0-9_]+$ ]]
}

deskbot_validate_ws_port() {
  local port=$1
  [[ "$port" =~ ^[0-9]+$ ]] && ((port > 0 && port <= 65535))
}

deskbot_validate_fps() {
  local fps=$1
  [[ "$fps" =~ ^[0-9]+$ ]] && ((fps >= 0 && fps <= 30))
}

deskbot_parse_fps() {
  local val="${1:-}" default="${2:-0}"
  if [[ -z "$val" ]]; then
    printf '%s' "$default"
    return 0
  fi
  if deskbot_validate_fps "$val"; then
    printf '%s' "$val"
    return 0
  fi
  return 1
}

deskbot_write_wifi_defaults_h() {
  local fw_dir=$1
  local ssid_c pw_c
  ssid_c="$(deskbot_escape_c_string "$WIFI_SSID")"
  pw_c="$(deskbot_escape_c_string "$WIFI_PASSWORD")"
  cat >"${fw_dir}/firmware/wifi_defaults.h" <<EOF
#pragma once

/* 自动生成 — 编辑仓库根目录 deskbot.local.env 中的 WIFI_SSID / WIFI_PASSWORD */

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID "${ssid_c}"
#endif

#ifndef WIFI_DEFAULT_PASSWORD
#define WIFI_DEFAULT_PASSWORD "${pw_c}"
#endif
EOF
}

deskbot_ensure_local_env() {
  if [[ ! -f "$DESKBOT_LOCAL_ENV" ]]; then
    if [[ ! -f "$DESKBOT_LOCAL_ENV_EXAMPLE" ]]; then
      echo "缺少 ${DESKBOT_LOCAL_ENV_EXAMPLE}，无法创建 deskbot.local.env" >&2
      exit 1
    fi
    cp "$DESKBOT_LOCAL_ENV_EXAMPLE" "$DESKBOT_LOCAL_ENV"
    echo "==> 已从 deskbot.local.env.example 复制到 ${DESKBOT_LOCAL_ENV}"
    echo "==> 请编辑 deskbot.local.env 后重新烧录（本次将使用 example 中的默认值继续）"
  fi

  deskbot_load_local_env

  if [[ -n "${DEVICE_ID:-}" ]] && ! deskbot_validate_device_id "$DEVICE_ID"; then
    echo "deskbot.local.env 中 DEVICE_ID 格式无效（需 deskbot_ 前缀）: $DEVICE_ID" >&2
    exit 1
  fi

  if [[ -z "${DESKBOT_WS_HOST:-}" ]]; then
    DESKBOT_WS_HOST="$DESKBOT_DEFAULT_WS_HOST"
  fi

  if [[ -z "${DESKBOT_WS_PORT:-}" ]]; then
    DESKBOT_WS_PORT="$DESKBOT_DEFAULT_WS_PORT"
  elif ! deskbot_validate_ws_port "$DESKBOT_WS_PORT"; then
    echo "deskbot.local.env 中 DESKBOT_WS_PORT 无效: $DESKBOT_WS_PORT" >&2
    exit 1
  fi

  local upload_jpeg local_face
  upload_jpeg="$(deskbot_parse_bool "${CAMERA_UPLOAD_JPEG:-}" 1)" || {
    echo "deskbot.local.env 中 CAMERA_UPLOAD_JPEG 无效（请用 0/1）: ${CAMERA_UPLOAD_JPEG:-}" >&2
    exit 1
  }
  local_face="$(deskbot_parse_bool "${CAMERA_LOCAL_FACE_DETECT:-}" 1)" || {
    echo "deskbot.local.env 中 CAMERA_LOCAL_FACE_DETECT 无效（请用 0/1）: ${CAMERA_LOCAL_FACE_DETECT:-}" >&2
    exit 1
  }
  CAMERA_UPLOAD_JPEG="$upload_jpeg"
  CAMERA_LOCAL_FACE_DETECT="$local_face"

  local upload_fps face_fps
  upload_fps="$(deskbot_parse_fps "${CAMERA_UPLOAD_FPS:-}" 10)" || {
    echo "deskbot.local.env 中 CAMERA_UPLOAD_FPS 无效（0–30）: ${CAMERA_UPLOAD_FPS:-}" >&2
    exit 1
  }
  face_fps="$(deskbot_parse_fps "${CAMERA_FACE_DETECT_FPS:-}" 5)" || {
    echo "deskbot.local.env 中 CAMERA_FACE_DETECT_FPS 无效（0–30）: ${CAMERA_FACE_DETECT_FPS:-}" >&2
    exit 1
  }
  CAMERA_UPLOAD_FPS="$upload_fps"
  CAMERA_FACE_DETECT_FPS="$face_fps"

  echo "==> device_id: ${DEVICE_ID:-未设置（固件使用 MAC 生成）} （deskbot.local.env）"
  echo "==> remote server: ${DESKBOT_WS_HOST}:${DESKBOT_WS_PORT} （deskbot.local.env）"
  if [[ -n "${WIFI_SSID:-}" ]]; then
    echo "==> WiFi SSID: ${WIFI_SSID} （deskbot.local.env）"
  else
    echo "==> WiFi: 未设置（ROM 配网热点 Deskbot_Rom；Camera 配网热点 Deskbot_Camera）"
  fi
}

# prepare_deskbot_flash_config <fw_dir> <pio_env> <module: rom|camera>
prepare_deskbot_flash_config() {
  local fw_dir=$1 pio_env=$2 module=$3
  deskbot_ensure_local_env
  deskbot_write_wifi_defaults_h "$fw_dir"

  if [[ "$module" == "rom" ]]; then
    {
      echo "; 自动生成 — 编辑仓库根目录 deskbot.local.env"
      echo "[env:${pio_env}]"
      echo "build_flags ="
      if [[ -n "${DEVICE_ID:-}" ]]; then
        printf "\t'-DDEVICE_ID=\"%s\"'\n" "$DEVICE_ID"
      fi
      printf "\t'-DASR_CHAT_HOST=\"%s\"'\n" "$DESKBOT_WS_HOST"
      printf "\t-DASR_CHAT_PORT=%s\n" "$DESKBOT_WS_PORT"
    } >"${fw_dir}/deskbot_build.local.ini"
  elif [[ "$module" == "camera" ]]; then
    local upload_jpeg local_face upload_fps face_fps
    upload_jpeg="$(deskbot_parse_bool "${CAMERA_UPLOAD_JPEG:-}" 1)"
    local_face="$(deskbot_parse_bool "${CAMERA_LOCAL_FACE_DETECT:-}" 1)"
    upload_fps="$(deskbot_parse_fps "${CAMERA_UPLOAD_FPS:-}" 10)"
    face_fps="$(deskbot_parse_fps "${CAMERA_FACE_DETECT_FPS:-}" 5)"
    echo "==> camera vision: upload_jpeg=${upload_jpeg} upload_fps=${upload_fps} local_face_detect=${local_face} face_fps=${face_fps} （deskbot.local.env）"
    if ! deskbot_env_has_key CAMERA_UPLOAD_JPEG || ! deskbot_env_has_key CAMERA_LOCAL_FACE_DETECT \
      || ! deskbot_env_has_key CAMERA_UPLOAD_FPS || ! deskbot_env_has_key CAMERA_FACE_DETECT_FPS; then
      echo "==> 提示: 未写明的 CAMERA_* 项使用默认值参与编译；关闭本地检测请写 CAMERA_LOCAL_FACE_DETECT=0"
    fi
    if [[ "$upload_jpeg" == 0 && "$local_face" == 0 ]]; then
      echo "==> 提示: 两项均为 0，摄像头不会连接 WebSocket 上行（本地 HTTP 预览仍可用）"
    elif [[ -z "${DESKBOT_WS_HOST:-}" ]]; then
      echo "==> 提示: DESKBOT_WS_HOST 为空，WebSocket 上行已禁用"
    fi
    {
      echo "; 自动生成 — 编辑仓库根目录 deskbot.local.env"
      echo "[env:${pio_env}]"
      echo "build_flags ="
      if [[ -n "${DEVICE_ID:-}" ]]; then
        printf "\t'-DDESKBOT_DEVICE_ID=\"%s\"'\n" "$DEVICE_ID"
      fi
      printf "\t'-DDESKBOT_WS_HOST=\"%s\"'\n" "$DESKBOT_WS_HOST"
      printf "\t-DDESKBOT_WS_PORT=%s\n" "$DESKBOT_WS_PORT"
      printf "\t-DDESKBOT_CAMERA_UPLOAD_JPEG=%s\n" "$upload_jpeg"
      printf "\t-DDESKBOT_CAMERA_LOCAL_FACE_DETECT=%s\n" "$local_face"
      printf "\t-DDESKBOT_CAMERA_UPLOAD_FPS=%s\n" "$upload_fps"
      printf "\t-DDESKBOT_CAMERA_FACE_DETECT_FPS=%s\n" "$face_fps"
    } >"${fw_dir}/deskbot_build.local.ini"
  else
    echo "未知模块: $module（应为 rom 或 camera）" >&2
    exit 1
  fi
}
