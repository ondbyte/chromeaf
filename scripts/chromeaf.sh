#!/usr/bin/env bash
# Copyright 2026 The chromeaf Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Build steps for chromeaf. Normally driven by the top-level Makefile; every
# step is idempotent (guarded by stamp files) and can be run on its own:
#
#   scripts/chromeaf.sh <step>
#
# Linux follows ungoogled-chromium-portablelinux's recipe (source tarball ->
# prune -> ungoogled + portablelinux patches -> chromeaf patches -> domain
# substitution -> toolchain -> gn -> ninja). Android uses a depot_tools
# checkout because the source tarball does not contain the Android SDK/NDK.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UC="${ROOT}/ungoogled-chromium"
PL="${ROOT}/platform/portablelinux"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build}"
SRC="${SRC:-${BUILD_DIR}/src}"
OUT="${OUT:-${SRC}/out/chromeaf}"
DL_CACHE="${BUILD_DIR}/download_cache"
FETCH="${FETCH:-tarball}"           # tarball | clone
BUILD_TYPE="${BUILD_TYPE:-release}" # release | dev
TARGET_CPU="${TARGET_CPU:-}"
JOBS="${JOBS:-$(nproc)}"
TARGETS="${TARGETS:-chrome chromeaf_unittests}"

ANDROID_ROOT="${ANDROID_ROOT:-${BUILD_DIR}/android}"
ANDROID_SRC="${ANDROID_ROOT}/src"
ANDROID_OUT="${ANDROID_OUT:-${ANDROID_SRC}/out/chromeaf-android}"
ANDROID_CPU="${ANDROID_CPU:-arm64}"
ANDROID_UC_PATCHES="${ANDROID_UC_PATCHES:-0}"
DEPOT_TOOLS="${DEPOT_TOOLS:-${BUILD_DIR}/depot_tools}"

CHROMIUM_VERSION="$(cat "${UC}/chromium_version.txt")"

log() { printf '\033[1;34m[chromeaf]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[chromeaf]\033[0m %s\n' "$*" >&2; exit 1; }

host_cpu() {
  case "$(uname -m)" in
    x86_64) echo x64 ;;
    aarch64) echo arm64 ;;
    *) uname -m ;;
  esac
}

# Runs "$@" once per source tree, remembered with a stamp file.
once() {
  local stamp="$1"; shift
  if [[ -f "${stamp}" ]]; then
    log "skip: $(basename "${stamp}") (delete it to redo)"
    return 0
  fi
  "$@"
  touch "${stamp}"
}

# ---------------------------------------------------------------------------
# Common
# ---------------------------------------------------------------------------

cmd_submodules() {
  git -C "${ROOT}" submodule update --init ungoogled-chromium platform/portablelinux
}

cmd_check_deps() {
  local missing=()
  local tools=(python3 git ninja node npm gperf go clang-format rsync xz)
  for t in "${tools[@]}"; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
  done
  if [[ ! -d /usr/lib/node_modules/esbuild ]]; then
    missing+=("esbuild (npm -g install esbuild)")
  fi
  if ((${#missing[@]})); then
    log "missing host tools: ${missing[*]}"
    log "see platform/portablelinux/docker/build.Dockerfile for the full list,"
    log "or build inside that container with 'make docker-linux'."
    return 1
  fi
  log "host tools OK"
}

cmd_depot_tools() {
  if [[ ! -d "${DEPOT_TOOLS}" ]]; then
    git clone --depth 1 \
      https://chromium.googlesource.com/chromium/tools/depot_tools.git \
      "${DEPOT_TOOLS}"
  fi
}

# Copies the chromeaf overlay into a source tree.
sync_overlay() {
  local src="$1"
  rsync -a --delete "${ROOT}/chromeaf/" "${src}/chromeaf/"
}

apply_chromeaf_patches() {
  local src="$1"
  python3 "${UC}/utils/patches.py" apply "${src}" "${ROOT}/patches"
}

# ---------------------------------------------------------------------------
# Linux
# ---------------------------------------------------------------------------

linux_fetch() {
  mkdir -p "${DL_CACHE}"
  if [[ "${FETCH}" == clone ]]; then
    local sysroot
    sysroot="$(host_cpu)"; [[ "${sysroot}" == x64 ]] && sysroot=amd64
    python3 "${UC}/utils/clone.py" --sysroot "${sysroot}" -o "${SRC}"
  else
    python3 "${UC}/utils/downloads.py" retrieve -i "${UC}/downloads.ini" \
      -c "${DL_CACHE}"
    python3 "${UC}/utils/downloads.py" unpack -i "${UC}/downloads.ini" \
      -c "${DL_CACHE}" "${SRC}"
  fi
}
cmd_fetch() {
  log "Chromium ${CHROMIUM_VERSION} (ungoogled-chromium $(cat "${UC}/revision.txt")) -> ${SRC}"
  mkdir -p "${BUILD_DIR}"
  once "${SRC}/.chromeaf-fetched" linux_fetch
}

linux_patch() {
  python3 "${UC}/utils/prune_binaries.py" "${SRC}" "${UC}/pruning.list"
  # ungoogled-chromium, then the portablelinux platform layer, then chromeaf.
  python3 "${UC}/utils/patches.py" apply "${SRC}" "${UC}/patches" \
    "${PL}/patches" "${ROOT}/patches"
}

linux_domsub() {
  python3 "${UC}/utils/domain_substitution.py" apply \
    -r "${UC}/domain_regex.list" -f "${UC}/domain_substitution.list" \
    -c "${BUILD_DIR}/domsubcache.tar.gz" "${SRC}"
}

# Domain substitution also rewrites the toolchain download scripts; restore
# the real hosts for those (same as portablelinux).
linux_fix_tool_downloading() {
  sed -i 's/commondatastorage.9oo91eapis.qjz9zk/commondatastorage.googleapis.com/g' \
    "${SRC}/build/linux/sysroot_scripts/sysroots.json" \
    "${SRC}/tools/clang/scripts/update.py" \
    "${SRC}/tools/clang/scripts/build.py"
  sed -i 's/chromium.9oo91esource.qjz9zk/chromium.googlesource.com/g' \
    "${SRC}/tools/clang/scripts/build.py" \
    "${SRC}/tools/rust/build_rust.py" \
    "${SRC}/tools/rust/build_bindgen.py"
  sed -i 's/chrome-infra-packages.8pp2p8t.qjz9zk/chrome-infra-packages.appspot.com/g' \
    "${SRC}/tools/rust/build_rust.py"
}

linux_toolchain() {
  if [[ "$(host_cpu)" == x64 ]]; then
    python3 "${SRC}/tools/rust/update_rust.py"
    python3 "${SRC}/tools/clang/scripts/update.py"
  else
    python3 "${SRC}/tools/clang/scripts/build.py" \
      --without-fuchsia --without-android --disable-asserts \
      --host-cc=clang --host-cxx=clang++ --use-system-cmake \
      --with-ml-inliner-model=
    CARGO_HOME="${SRC}/third_party/rust-src/cargo-home" \
      python3 "${SRC}/tools/rust/build_rust.py" --skip-test
    python3 "${SRC}/tools/rust/build_bindgen.py"
  fi
  python3 "${SRC}/build/linux/sysroot_scripts/install-sysroot.py" \
    --arch="$(host_cpu)"
  if [[ -n "${TARGET_CPU}" && "${TARGET_CPU}" != "$(host_cpu)" ]]; then
    python3 "${SRC}/build/linux/sysroot_scripts/install-sysroot.py" \
      --arch="${TARGET_CPU}"
  fi

  # Host tools the pruned tree expects at fixed locations.
  mkdir -p "${SRC}/third_party/node/linux/node-linux-x64/bin" \
    "${SRC}/third_party/gperf/cipd/bin" \
    "${SRC}/third_party/dawn/tools/golang/linux-amd64/bin" \
    "${SRC}/buildtools/linux64-format"
  ln -sf "$(command -v node)" "${SRC}/third_party/node/linux/node-linux-x64/bin/node"
  ln -sf "$(command -v gperf)" "${SRC}/third_party/gperf/cipd/bin/gperf"
  ln -sf "$(command -v go)" "${SRC}/third_party/dawn/tools/golang/linux-amd64/bin/go"
  ln -sf "$(command -v clang-format)" "${SRC}/buildtools/linux64-format/clang-format"
  rm -rf "${SRC}/third_party/devtools-frontend/src/node_modules/esbuild"
  ln -s /usr/lib/node_modules/esbuild \
    "${SRC}/third_party/devtools-frontend/src/node_modules/esbuild"
}

cmd_prepare() {
  [[ -f "${SRC}/.chromeaf-fetched" ]] || die "run 'make fetch' first"
  # The overlay must exist before patching: patched BUILD files reference it.
  sync_overlay "${SRC}"
  once "${SRC}/.chromeaf-patched" linux_patch
  once "${SRC}/.chromeaf-domsub" linux_domsub
  once "${SRC}/.chromeaf-tools-fixed" linux_fix_tool_downloading
  once "${SRC}/.chromeaf-toolchain" linux_toolchain
}

cmd_overlay() {
  sync_overlay "${SRC}"
  log "overlay synced into ${SRC}/chromeaf"
}

# Exports CC/CXX/... for the bundled clang, as portablelinux does.
export_toolchain_env() {
  local bin="${SRC}/third_party/llvm-build/Release+Asserts/bin"
  export CC="${bin}/clang" CXX="${bin}/clang++" AR="${bin}/llvm-ar"
  export NM="${bin}/llvm-nm" LLVM_BIN="${bin}"
  local resource_dir
  resource_dir="$("${CC}" --print-resource-dir)"
  export CFLAGS="${CFLAGS:-} -resource-dir=${resource_dir} -B${bin}"
  export CXXFLAGS="${CXXFLAGS:-} -resource-dir=${resource_dir} -B${bin}"
  export CPPFLAGS="${CPPFLAGS:-} -resource-dir=${resource_dir} -B${bin}"
}

cmd_gn() {
  sync_overlay "${SRC}"
  mkdir -p "${OUT}"
  local cpu="${TARGET_CPU:-$(host_cpu)}"
  {
    echo "# Generated by scripts/chromeaf.sh; edit build/args/*.gn instead."
    cat "${UC}/flags.gn"
    cat "${PL}/flags.linux.gn"
    cat "${ROOT}/build/args/linux.gn"
    if [[ "${BUILD_TYPE}" == dev ]]; then
      cat "${ROOT}/build/args/dev.gn"
    fi
    echo "target_cpu = \"${cpu}\""
    echo "v8_target_cpu = \"${cpu}\""
    if [[ -f "${ROOT}/build/args/local.gn" ]]; then
      cat "${ROOT}/build/args/local.gn"
    fi
  } > "${OUT}/args.gn.new"
  # GN args are "last one wins"; drop earlier duplicates so gn doesn't warn.
  python3 - "${OUT}/args.gn.new" "${OUT}/args.gn" <<'PY'
import re, sys
lines = open(sys.argv[1]).read().splitlines()
seen, out = set(), []
for line in reversed(lines):
    m = re.match(r'\s*([A-Za-z0-9_]+)\s*=', line)
    if m:
        if m.group(1) in seen:
            continue
        seen.add(m.group(1))
    out.append(line)
open(sys.argv[2], 'w').write('\n'.join(reversed(out)) + '\n')
PY
  rm -f "${OUT}/args.gn.new"

  export_toolchain_env
  if [[ ! -x "${OUT}/gn" ]]; then
    (cd "${SRC}" && python3 tools/gn/bootstrap/bootstrap.py -o "${OUT}/gn" \
      --skip-generate-buildfiles)
  fi
  (cd "${SRC}" && "${OUT}/gn" gen "${OUT}" --fail-on-unused-args)
}

cmd_build() {
  [[ -f "${OUT}/build.ninja" ]] || cmd_gn
  sync_overlay "${SRC}"
  export_toolchain_env
  # shellcheck disable=SC2086
  ninja -C "${OUT}" -j "${JOBS}" ${TARGETS}
}

cmd_unittests() {
  "${OUT}/chromeaf_unittests" "$@"
}

cmd_package() {
  local version="${CHROMIUM_VERSION}-$(cat "${UC}/revision.txt")"
  local name="chromeaf-${version}-$(host_cpu)-linux"
  local dir="${BUILD_DIR}/release/${name}"
  rm -rf "${dir}"
  mkdir -p "${dir}"
  local files=(chrome chrome_100_percent.pak chrome_200_percent.pak
    chrome_crashpad_handler icudtl.dat libEGL.so libGLESv2.so
    libvk_swiftshader.so libvulkan.so.1 locales resources.pak
    v8_context_snapshot.bin vk_swiftshader_icd.json)
  for f in "${files[@]}" libqt5_shim.so libqt6_shim.so; do
    if [[ -e "${OUT}/${f}" ]]; then
      cp -r "${OUT}/${f}" "${dir}/"
    fi
  done
  cat > "${dir}/chromeaf" <<'SH'
#!/bin/sh
# Launches chromeaf. Set CHROMEAF_MCP_PORT to enable the MCP control server.
HERE="$(dirname "$(readlink -f "$0")")"
if [ -n "${CHROMEAF_MCP_PORT:-}" ]; then
  set -- --chromeaf-mcp-port="${CHROMEAF_MCP_PORT}" "$@"
fi
exec "${HERE}/chrome" "$@"
SH
  chmod +x "${dir}/chromeaf"
  tar -C "${BUILD_DIR}/release" -cJf "${BUILD_DIR}/release/${name}.tar.xz" \
    "${name}"
  log "package: ${BUILD_DIR}/release/${name}.tar.xz"
}

# ---------------------------------------------------------------------------
# Android (experimental)
# ---------------------------------------------------------------------------

android_fetch() {
  cmd_depot_tools
  export PATH="${DEPOT_TOOLS}:${PATH}"
  export DEPOT_TOOLS_UPDATE=0
  mkdir -p "${ANDROID_ROOT}"
  cat > "${ANDROID_ROOT}/.gclient" <<GCLIENT
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {"checkout_configuration": "small"},
  },
]
target_os = ["android"]
GCLIENT
  (cd "${ANDROID_ROOT}" && gclient sync --no-history --shallow \
    --revision "src@refs/tags/${CHROMIUM_VERSION}" --nohooks)
  (cd "${ANDROID_ROOT}" && gclient runhooks)
}
cmd_android_fetch() {
  once "${ANDROID_ROOT}/.chromeaf-fetched" android_fetch
}

android_patch() {
  if [[ "${ANDROID_UC_PATCHES}" == 1 ]]; then
    # ungoogled-chromium does not officially support Android; its patches
    # are opt-in here.
    python3 "${UC}/utils/patches.py" apply "${ANDROID_SRC}" "${UC}/patches"
  fi
  apply_chromeaf_patches "${ANDROID_SRC}"
}
cmd_android_prepare() {
  [[ -f "${ANDROID_ROOT}/.chromeaf-fetched" ]] || die "run 'make android-fetch' first"
  sync_overlay "${ANDROID_SRC}"
  once "${ANDROID_ROOT}/.chromeaf-patched" android_patch
}

cmd_android_gn() {
  export PATH="${DEPOT_TOOLS}:${PATH}"
  export DEPOT_TOOLS_UPDATE=0
  sync_overlay "${ANDROID_SRC}"
  mkdir -p "${ANDROID_OUT}"
  {
    cat "${ROOT}/build/args/android.gn"
    if [[ "${ANDROID_UC_PATCHES}" == 1 ]]; then
      cat "${UC}/flags.gn"
    fi
    echo "target_cpu = \"${ANDROID_CPU}\""
  } > "${ANDROID_OUT}/args.gn"
  (cd "${ANDROID_SRC}" && gn gen "${ANDROID_OUT}")
}

cmd_android_build() {
  export PATH="${DEPOT_TOOLS}:${PATH}"
  export DEPOT_TOOLS_UPDATE=0
  [[ -f "${ANDROID_OUT}/build.ninja" ]] || cmd_android_gn
  sync_overlay "${ANDROID_SRC}"
  (cd "${ANDROID_SRC}" && autoninja -C "${ANDROID_OUT}" chrome_public_apk)
}

# ---------------------------------------------------------------------------

usage() {
  sed -n '/^cmd_/s/^cmd_\([a-z_]*\)().*/  \1/p' "${BASH_SOURCE[0]}" | tr _ -
}

main() {
  local step="${1:-}"
  [[ -n "${step}" ]] || { echo "steps:"; usage; exit 1; }
  shift
  local fn="cmd_${step//-/_}"
  declare -F "${fn}" >/dev/null || { echo "unknown step: ${step}"; usage; exit 1; }
  "${fn}" "$@"
}

main "$@"
