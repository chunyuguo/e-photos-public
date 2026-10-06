#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_ROOT="${IDF_PATH:-}"

usage() {
  cat >&2 <<'EOF'
用法: ./flash.sh [c5_v1.3|c6_v1.3|c5_v3.2|c6_v3.2] [flash|monitor|flash-monitor] [串口]

未指定目标时默认 c6_v1.3；旧写法 c5/c6 分别等同于 c5_v1.3/c6_v1.3。
EOF
}

normalize_profile() {
  case "${1,,}" in
    c5|c5_v1.3) printf 'c5_v1.3\n' ;;
    c6|c6_v1.3) printf 'c6_v1.3\n' ;;
    c5_v3.2) printf 'c5_v3.2\n' ;;
    c6_v3.2) printf 'c6_v3.2\n' ;;
    *) return 1 ;;
  esac
}

detect_port() {
  local port
  port="$(ls -1t /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1 || true)"
  [[ -n "${port}" ]] || { echo "[ERROR] 未检测到开发板串口" >&2; exit 2; }
  printf '%s\n' "${port}"
}

[[ -n "${IDF_ROOT}" && -f "${IDF_ROOT}/export.sh" ]] || { echo "[ERROR] 请先设置 IDF_PATH 指向 ESP-IDF 安装目录" >&2; exit 1; }
profile="$(normalize_profile "${1:-c6_v1.3}")" || { usage; exit 3; }
shift || true
action="${1:-flash}"
[[ $# -gt 0 ]] && shift

if [[ "${action}" == "-h" || "${action}" == "--help" ]]; then
  usage
  exit 0
fi

# shellcheck disable=SC1091
source "${IDF_ROOT}/export.sh"
export PYTHONWARNINGS="${PYTHONWARNINGS:-ignore}"
cd "${PROJECT_DIR}"

build_dir="${PROJECT_DIR}/build_${profile}"
[[ -d "${build_dir}" ]] || { echo "[ERROR] 未找到 ${build_dir}，请先执行 ./build.sh ${profile}" >&2; exit 4; }

if [[ "${profile}" == *_v3.2 ]]; then
  esptool_version="$("${ESP_PYTHON:-python}" -c 'from importlib.metadata import version; print(version("esptool"))')"
  if ! "${ESP_PYTHON:-python}" -c 'from importlib.metadata import version; from packaging.version import Version; raise SystemExit(Version(version("esptool")) < Version("4.12.0"))'; then
    echo "[ERROR] ${profile} (ESP32-P4 ECO7/rev 3.2) 需要官方 esptool >= 4.12.0，当前为 ${esptool_version}。" >&2
    echo "        请在 IDF 5.5 环境中执行: python -m pip install --upgrade 'esptool==4.12.0'" >&2
    exit 5
  fi
fi

port="${1:-$(detect_port)}"
flash_baud="${EPHOTO_FLASH_BAUD:-115200}"

case "${action}" in
  # Use a conservative UART rate for P4 ECO7; override with EPHOTO_FLASH_BAUD if validated.
  flash) idf.py -B "${build_dir}" -p "${port}" -b "${flash_baud}" flash ;;
  monitor) idf.py -B "${build_dir}" -p "${port}" monitor ;;
  flash-monitor) idf.py -B "${build_dir}" -p "${port}" -b "${flash_baud}" flash monitor ;;
  *) usage; exit 3 ;;
esac
