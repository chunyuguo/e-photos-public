#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_ROOT="${IDF_PATH:-}"
VERSION_FILE="${PROJECT_DIR}/VERSION"
RELEASE_DIR="${PROJECT_DIR}/build_all"
PROFILES=(c5_v1.3 c6_v1.3 c5_v3.2 c6_v3.2)

if [[ -z "${IDF_ROOT}" || ! -f "${IDF_ROOT}/export.sh" ]]; then
  echo "[ERROR] 请先设置 IDF_PATH 指向 ESP-IDF 安装目录" >&2
  exit 1
fi

usage() {
  cat >&2 <<'EOF'
用法: ./build.sh [all|c5_v1.3|c6_v1.3|c5_v3.2|c6_v3.2] [idf.py 参数...]

不带参数时构建全部四个固件。全部成功后，只发布四个整片 16MB 烧录包到 build_all/。
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

configure_profile() {
  local profile="$1"
  case "${profile}" in
    c5_v1.3) CP_TARGET=c5; HW_REVISION=v1.3 ;;
    c6_v1.3) CP_TARGET=c6; HW_REVISION=v1.3 ;;
    c5_v3.2) CP_TARGET=c5; HW_REVISION=v3.2 ;;
    c6_v3.2) CP_TARGET=c6; HW_REVISION=v3.2 ;;
  esac

  BUILD_DIR="${PROJECT_DIR}/build_${profile}"
  SDKCONFIG_FILE="${BUILD_DIR}/sdkconfig"
  SDKCONFIG_DEFAULTS_VALUE="sdkconfig.defaults;sdkconfig.defaults.p4_${HW_REVISION};sdkconfig.defaults.${CP_TARGET}"
  ARTIFACT_TARGET_LABEL="p4${CP_TARGET}_${HW_REVISION}"
  BUILD_VERSION_STAMP="${BUILD_DIR}/.ephoto_project_ver"
}

ensure_project_version_configured() {
  local current_version recorded_version
  current_version="$(tr -d '\r\n' < "${VERSION_FILE}")"
  recorded_version=""

  if [[ -f "${BUILD_VERSION_STAMP}" ]]; then
    recorded_version="$(tr -d '\r\n' < "${BUILD_VERSION_STAMP}")"
  fi

  [[ "${current_version}" == "${recorded_version}" ]] && return

  echo "[INFO] ${profile}: 检测到项目版本变更: ${recorded_version:-<none>} -> ${current_version}，执行 reconfigure"
  # This file is generated from the profile defaults. Discard it on a release
  # version change so newly introduced Kconfig defaults (such as OTA rollback)
  # cannot be masked by a stale per-profile configuration cache.
  rm -f "${SDKCONFIG_FILE}"
  idf.py -B "${BUILD_DIR}" \
    -DIDF_TARGET=esp32p4 \
    -DSDKCONFIG="${SDKCONFIG_FILE}" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS_VALUE}" \
    -DEPHOTO_COPROCESSOR="${CP_TARGET^^}" \
    -DEPHOTO_HW_REVISION="${HW_REVISION}" \
    reconfigure
  printf '%s\n' "${current_version}" > "${BUILD_VERSION_STAMP}"
}

package_release_artifacts() {
  local current_version full_target full_name flash_args_file
  current_version="$(tr -d '\r\n' < "${VERSION_FILE}")"
  full_name="e-photo_${current_version}_${ARTIFACT_TARGET_LABEL}_full_16mb.bin"
  full_target="${BUILD_DIR}/${full_name}"
  flash_args_file="${BUILD_DIR}/flash_args"

  [[ -f "${BUILD_DIR}/e_photo.bin" && -f "${flash_args_file}" ]] || {
    echo "[ERROR] ${profile} 未生成完整烧录产物" >&2
    return 1
  }

  (cd "${BUILD_DIR}" && python -m esptool --chip esp32p4 merge_bin -o "${full_name}" @flash_args)
  # e_photo.bin is only the intermediate application image used by merge_bin;
  # the public release directory contains full flash images only.
  rm -f "${BUILD_DIR}/e_photo.bin"
  find "${BUILD_DIR}" -maxdepth 1 -type f -name '*_ota.bin' -delete
  echo "[OK] ${profile}: ${full_target}"
}

should_package_full_artifact() {
  local arg
  for arg in "$@"; do
    case "${arg}" in
      build|all) return 0 ;;
    esac
  done
  return 1
}

publish_all_release_artifacts() {
  local profile current_version full_name source staging_dir previous_dir
  current_version="$(tr -d '\r\n' < "${VERSION_FILE}")"
  staging_dir="$(mktemp -d "${PROJECT_DIR}/.build_all.staging.XXXXXX")"
  previous_dir="${PROJECT_DIR}/.build_all.previous.$$"

  for profile in "${PROFILES[@]}"; do
    configure_profile "${profile}"
    full_name="e-photo_${current_version}_${ARTIFACT_TARGET_LABEL}_full_16mb.bin"

    for source in "${BUILD_DIR}/${full_name}"; do
      if [[ ! -f "${source}" ]]; then
        echo "[ERROR] ${profile} 缺少发布文件: ${source}" >&2
        rm -rf "${staging_dir}"
        return 1
      fi
      cp -f "${source}" "${staging_dir}/"
    done
  done

  # Publish only a complete, successfully built set; the old set is removed afterwards.
  if [[ -e "${RELEASE_DIR}" ]]; then
    mv "${RELEASE_DIR}" "${previous_dir}"
  fi
  mv "${staging_dir}" "${RELEASE_DIR}"
  rm -rf "${previous_dir}"
  echo "[OK] 已发布最新四套全量固件（4 个文件）: ${RELEASE_DIR}"
}

build_profile() {
  local profile="$1"
  shift
  configure_profile "${profile}"
  mkdir -p "${BUILD_DIR}"
  echo "[INFO] 构建 ${profile} (P4 ${HW_REVISION}, ${CP_TARGET^^})"
  ensure_project_version_configured
  # package_release_artifacts removes this intermediate image after a
  # successful build.  If the profile is built again, force CMake/Make to
  # recreate it instead of reusing a stale target state from the previous run.
  if should_package_full_artifact "$@" && [[ ! -f "${BUILD_DIR}/e_photo.bin" ]]; then
    idf.py -B "${BUILD_DIR}" clean
  fi
  idf.py -B "${BUILD_DIR}" \
    -DIDF_TARGET=esp32p4 \
    -DSDKCONFIG="${SDKCONFIG_FILE}" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS_VALUE}" \
    -DEPHOTO_COPROCESSOR="${CP_TARGET^^}" \
    -DEPHOTO_HW_REVISION="${HW_REVISION}" \
    "$@"
  should_package_full_artifact "$@" && package_release_artifacts
}

# shellcheck disable=SC1091
source "${IDF_ROOT}/export.sh"
export PYTHONWARNINGS="${PYTHONWARNINGS:-ignore}"
cd "${PROJECT_DIR}"

requested="${1:-all}"
if [[ $# -gt 0 ]]; then shift; fi
idf_args=("$@")
[[ ${#idf_args[@]} -gt 0 ]] || idf_args=(build)

case "${requested,,}" in
  all)
    for profile in "${PROFILES[@]}"; do
      build_profile "${profile}" "${idf_args[@]}"
    done
    should_package_full_artifact "${idf_args[@]}" && publish_all_release_artifacts
    ;;
  -h|--help) usage ;;
  *)
    profile="$(normalize_profile "${requested}")" || { usage; exit 3; }
    build_profile "${profile}" "${idf_args[@]}"
    ;;
esac
