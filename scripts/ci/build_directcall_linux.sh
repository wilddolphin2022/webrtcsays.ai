#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${ROOT_DIR}/out/linux_release"
WHILLATS_DIR="${ROOT_DIR}/modules/third_party/whillats"
WHILLATS_BUILD_DIR="${WHILLATS_DIR}/build"
DIST_DIR="${ROOT_DIR}/dist/directcall-linux"

echo "[build] root: ${ROOT_DIR}"

export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt-get install -y \
  autoconf \
  automake \
  build-essential \
  ca-certificates \
  clang \
  cmake \
  curl \
  git \
  libasound2-dev \
  libtool \
  libgtk-3-dev \
  libxtst6 \
  lld \
  m4 \
  ninja-build \
  pkg-config \
  python3 \
  rsync \
  unzip

cd "${ROOT_DIR}"

if [ ! -d "${HOME}/depot_tools" ]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "${HOME}/depot_tools"
fi
"${HOME}/depot_tools/ensure_bootstrap" || true
export PATH="${HOME}/depot_tools:${PATH}"

if [ -f ".gitmodules" ]; then
  git submodule update --init --recursive modules/third_party/whillats
fi

if [ ! -f "build/config/BUILDCONFIG.gn" ]; then
  echo "[build] build/config/BUILDCONFIG.gn missing; cloning chromium build repo"
  rm -rf build
  git clone --depth=1 https://chromium.googlesource.com/chromium/src/build.git build
fi

if [ ! -f "testing/test.gni" ]; then
  echo "[build] testing/test.gni missing; cloning chromium testing repo"
  rm -rf testing
  git clone --depth=1 https://chromium.googlesource.com/chromium/src/testing.git testing
fi

if [ ! -f "buildtools/third_party/libc++/BUILD.gn" ]; then
  echo "[build] buildtools libc++ missing; syncing from gclient src/buildtools"
  mkdir -p buildtools/third_party
  cp -a /root/src/buildtools/third_party/libc++ buildtools/third_party/libc++
  cp -a /root/src/buildtools/third_party/libc++abi buildtools/third_party/libc++abi
  cp -a /root/src/buildtools/third_party/libunwind buildtools/third_party/libunwind
fi

if [ ! -f "build/config/gclient_args.gni" ]; then
  echo "[build] creating fallback build/config/gclient_args.gni"
  mkdir -p build/config
  cat > build/config/gclient_args.gni <<'EOF'
# Generated for CI fallback
generate_location_tags = true
EOF
fi

if [ ! -f "build_overrides/protobuf.gni" ]; then
  echo "[build] creating fallback build_overrides/protobuf.gni"
  mkdir -p build_overrides
  cat > build_overrides/protobuf.gni <<'EOF'
# Chromium standalone fallback: no protobuf overrides.
EOF
fi

if [ ! -f "build_overrides/build.gni" ]; then
  echo "[build] creating fallback build_overrides/build.gni"
  mkdir -p build_overrides
  cat > build_overrides/build.gni <<'EOF'
# Chromium standalone fallback overrides.
use_libcxx_modules = false
EOF
fi
if ! rg "^use_libcxx_modules" "build_overrides/build.gni" >/dev/null 2>&1; then
  echo "use_libcxx_modules = false" >> build_overrides/build.gni
fi
for v in host_toolchain_is_msan host_toolchain_is_asan host_toolchain_is_tsan host_toolchain_is_ubsan; do
  if ! rg "^${v}\\s*=" "build_overrides/build.gni" >/dev/null 2>&1; then
    echo "${v} = false" >> build_overrides/build.gni
  fi
done

if [ ! -f "third_party/perfetto/include/perfetto/tracing/BUILD.gn" ]; then
  echo "[build] perfetto missing; cloning chromium perfetto repo"
  rm -rf third_party/perfetto
  mkdir -p third_party
  git clone --depth=1 https://android.googlesource.com/platform/external/perfetto third_party/perfetto
fi

if [ ! -f "third_party/abseil-cpp/BUILD.gn" ]; then
  echo "[build] abseil-cpp missing; cloning chromium abseil-cpp repo"
  rm -rf third_party/abseil-cpp
  mkdir -p third_party
  git clone --depth=1 https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp third_party/abseil-cpp
fi

if [ -x "build/install-build-deps.sh" ]; then
  ./build/install-build-deps.sh --no-chromeos-fonts || true
fi

if [ -f "build/linux/sysroot_scripts/install-sysroot.py" ]; then
  python3 build/linux/sysroot_scripts/install-sysroot.py --arch=amd64 || true
fi

if [ -f "tools/clang/scripts/update.py" ]; then
  python3 tools/clang/scripts/update.py
else
  echo "[build] skipping clang updater; tools/clang/scripts/update.py not present"
fi

if [ ! -f "build/dotfile_settings.gni" ]; then
  echo "[build] creating fallback build/dotfile_settings.gni"
  mkdir -p build
  cat > build/dotfile_settings.gni <<'EOF'
build_dotfile_settings = {
  exec_script_whitelist = []
}
EOF
fi

if [ -d ".git" ]; then
  git checkout -- .gn || true
fi

if [ -f ".gn" ]; then
  cp ".gn" ".gn.ci.bak"
  python3 - <<'PY'
from pathlib import Path
p = Path(".gn")
lines = p.read_text().splitlines()
lines = [ln for ln in lines if not ln.strip().startswith("export_compile_commands")]
if not any("exec_script_allowlist" in ln for ln in lines):
  lines.append("exec_script_allowlist = exec_script_whitelist")
p.write_text("\n".join(lines) + "\n")
PY
  echo "[build] patched .gn for CI gn compatibility"
fi

if [ -f "BUILD.gn" ] && rg '^\s*"sdk",\s*$' "BUILD.gn" >/dev/null 2>&1; then
  cp "BUILD.gn" "BUILD.gn.ci.bak"
  python3 - <<'PY'
from pathlib import Path
p = Path("BUILD.gn")
lines = p.read_text().splitlines()
lines = [ln for ln in lines if ln.strip() != '"sdk",']
p.write_text("\n".join(lines) + "\n")
PY
  echo "[build] patched BUILD.gn to skip sdk aggregate target"
fi

cmake -S "${WHILLATS_DIR}" -B "${WHILLATS_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=OFF \
  -DWHISPER_CUDA=OFF \
  -DGGML_CUBLAS=OFF \
  -DLLAMA_CUDA=OFF
cmake --build "${WHILLATS_BUILD_DIR}" --config Release --parallel 4

GN_BIN="${HOME}/depot_tools/gn"
if [ -x "${GN_BIN}" ]; then
  echo "[build] using gn from depot_tools"
elif [ -x "${ROOT_DIR}/buildtools/linux64/gn" ]; then
  GN_BIN="${ROOT_DIR}/buildtools/linux64/gn"
  echo "[build] using gn at ${GN_BIN}"
elif command -v gn >/dev/null 2>&1; then
  GN_BIN="gn"
  echo "[build] using gn from PATH"
else
  echo "[build] gn not found in depot_tools, PATH, or buildtools/linux64/gn"
  echo "[build] downloading prebuilt gn binary"
  curl -fsSL "https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/latest" -o /tmp/gn-linux-amd64.zip
  mkdir -p /tmp/gn-linux-amd64
  unzip -oq /tmp/gn-linux-amd64.zip -d /tmp/gn-linux-amd64
  chmod +x /tmp/gn-linux-amd64/gn
  GN_BIN="/tmp/gn-linux-amd64/gn"
fi

if ! "${GN_BIN}" --version >/dev/null 2>&1; then
  echo "[build] selected gn binary is not runnable; downloading prebuilt gn"
  curl -fsSL "https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/latest" -o /tmp/gn-linux-amd64.zip
  mkdir -p /tmp/gn-linux-amd64
  unzip -oq /tmp/gn-linux-amd64.zip -d /tmp/gn-linux-amd64
  chmod +x /tmp/gn-linux-amd64/gn
  GN_BIN="/tmp/gn-linux-amd64/gn"
fi

"${GN_BIN}" gen "${OUT_DIR}" --args='target_os="linux" is_debug=false is_clang=false rtc_include_opus=true rtc_include_tests=false rtc_build_examples=false rtc_build_sdk=false rtc_enable_symbol_export=true rtc_use_speech_audio_devices=true use_custom_libcxx=false enable_js_protobuf=false'
ninja -C "${OUT_DIR}" directcall

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/lib"
cp "${OUT_DIR}/directcall" "${DIST_DIR}/directcall"

if [ -d "${WHILLATS_BUILD_DIR}/lib/Release" ]; then
  cp "${WHILLATS_BUILD_DIR}/lib/Release/"*.so* "${DIST_DIR}/lib/" 2>/dev/null || true
fi
if [ -d "${WHILLATS_BUILD_DIR}/bin/Release" ]; then
  cp "${WHILLATS_BUILD_DIR}/bin/Release/"*.so* "${DIST_DIR}/lib/" 2>/dev/null || true
fi

cat > "${DIST_DIR}/run-directcall.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${SELF_DIR}/lib:${LD_LIBRARY_PATH:-}"
exec "${SELF_DIR}/directcall" "$@"
EOF
chmod +x "${DIST_DIR}/run-directcall.sh"

tar -C "${ROOT_DIR}/dist" -czf "${ROOT_DIR}/dist/directcall-linux.tar.gz" directcall-linux
echo "[build] packaged artifact: ${ROOT_DIR}/dist/directcall-linux.tar.gz"
