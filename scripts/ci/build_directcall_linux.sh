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
  echo "[build] buildtools libc++ missing; preparing fallback buildtools third_party"
  mkdir -p buildtools/third_party
  if [ -d "/root/src/buildtools/third_party/libc++" ]; then
    echo "[build] using buildtools snapshot from /root/src"
    cp -a /root/src/buildtools/third_party/libc++ buildtools/third_party/libc++
    cp -a /root/src/buildtools/third_party/libc++abi buildtools/third_party/libc++abi
    cp -a /root/src/buildtools/third_party/libunwind buildtools/third_party/libunwind
  else
    echo "[build] /root/src snapshot not found; cloning chromium buildtools"
    rm -rf /tmp/chromium-buildtools
    git clone --depth=1 https://chromium.googlesource.com/chromium/src/buildtools /tmp/chromium-buildtools
    cp -a /tmp/chromium-buildtools/third_party/libc++ buildtools/third_party/libc++
    cp -a /tmp/chromium-buildtools/third_party/libc++abi buildtools/third_party/libc++abi
    cp -a /tmp/chromium-buildtools/third_party/libunwind buildtools/third_party/libunwind
  fi
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
if ! rg "^not_fuzzed_add_configs\\s*=" "build_overrides/build.gni" >/dev/null 2>&1; then
  echo "not_fuzzed_add_configs = []" >> build_overrides/build.gni
fi
for v in \
  not_fuzzed_needs_asan_add_configs \
  not_fuzzed_needs_asan_remove_configs \
  not_fuzzed_remove_nonasan_configs; do
  if ! rg "^${v}\\s*=" "build_overrides/build.gni" >/dev/null 2>&1; then
    echo "${v} = []" >> build_overrides/build.gni
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

echo "[build] creating stub .gni files for third_party deps not cloned via gclient"
mkdir -p third_party/libsrtp third_party/libaom third_party/protobuf third_party/grpc third_party/jni_zero

if [ ! -f "third_party/libsrtp/BUILD.gn" ]; then
  echo "[build] cloning libsrtp"
  mkdir -p third_party
  git clone --depth=1 https://chromium.googlesource.com/chromium/deps/libsrtp.git third_party/libsrtp
fi

if [ ! -f "third_party/libaom/options.gni" ]; then
  echo "[build] creating libaom options stub"
  mkdir -p third_party/libaom
  cat > third_party/libaom/options.gni <<'GNI'
declare_args() {
  enable_libaom = false
  enable_libaom_decoder = false
}
GNI
fi

for stub in \
  "third_party/protobuf/proto_library.gni" \
  "third_party/grpc/grpc_library.gni" \
  "third_party/jni_zero/jni_zero.gni"; do
  if [ ! -f "${stub}" ]; then
    echo "[build] creating stub: ${stub}"
    mkdir -p "$(dirname "${stub}")"
    echo "# CI stub — feature disabled via GN args" > "${stub}"
  fi
done

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

echo "[build] ensuring build/dotfile_settings.gni has both allowlist and whitelist"
mkdir -p build
cat > build/dotfile_settings.gni <<'EOF'
build_dotfile_settings = {
  exec_script_whitelist = []
  exec_script_allowlist = []
}
EOF

if [ -d ".git" ]; then
  git checkout -- .gn || true
  git checkout -- BUILD.gn || true
  git checkout -- build/config/compiler/BUILD.gn || true
  git checkout -- build_overrides/build.gni || true
fi

if [ -f ".gn" ]; then
  cp ".gn" ".gn.ci.bak"
  python3 - <<'PY'
from pathlib import Path
import re

p = Path(".gn")
text = p.read_text()

# Remove import of dotfile_settings (not needed once we strip its uses)
text = re.sub(r'import\("//build/dotfile_settings\.gni"\)\n?', '', text)

# Remove exec_script_whitelist block (may span multiple lines with + continuation)
text = re.sub(r'exec_script_whitelist\s*=\s*[^\n]*(?:\n\s*[^\n]*\])?', '', text)

# Remove export_compile_commands
text = re.sub(r'export_compile_commands\s*=\s*\[[^\]]*\]\n?', '', text)

# Remove no_check_targets block (multi-line list)
text = re.sub(r'no_check_targets\s*=\s*\[.*?\]\n?', '', text, flags=re.DOTALL)

# Remove any remaining build_dotfile_settings references
text = re.sub(r'[^\n]*build_dotfile_settings[^\n]*\n?', '', text)

p.write_text(text)
PY
  echo "[build] patched .gn: stripped dotfile_settings, exec_script, no_check_targets for CI"
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
if [ -f "BUILD.gn" ] && rg '^\s*"rtc_tools",\s*$' "BUILD.gn" >/dev/null 2>&1; then
  cp "BUILD.gn" "BUILD.gn.ci.bak2"
  python3 - <<'PY'
from pathlib import Path
p = Path("BUILD.gn")
lines = p.read_text().splitlines()
lines = [ln for ln in lines if ln.strip() != '"rtc_tools",']
p.write_text("\n".join(lines) + "\n")
PY
  echo "[build] patched BUILD.gn to skip rtc_tools aggregate target"
fi

if [ -f "build/config/compiler/BUILD.gn" ]; then
  python3 - <<'PY'
from pathlib import Path
import re

p = Path("build/config/compiler/BUILD.gn")
text = p.read_text()

def normalize_config(name: str, body: str, text: str) -> str:
  pat = re.compile(rf'\nconfig\("{re.escape(name)}"\)\s*\{{.*?\n\}}\n', re.S)
  matches = pat.findall(text)
  if len(matches) == 0:
    text += "\n" + body + "\n"
  elif len(matches) > 1:
    text = pat.sub("\n", text)
    text += "\n" + matches[0].strip() + "\n"
  return text

text = normalize_config(
  "no_exit_time_destructors",
  'config("no_exit_time_destructors") {\n'
  '  if (is_clang) {\n'
  '    cflags = [ "-Wno-exit-time-destructors" ]\n'
  '  }\n'
  '}',
  text,
)
text = normalize_config(
  "no_global_constructors",
  'config("no_global_constructors") {\n'
  '  if (is_clang) {\n'
  '    cflags = [ "-Wno-global-constructors" ]\n'
  '  }\n'
  '}',
  text,
)

p.write_text(text)
PY
  echo "[build] normalized compiler config aliases for standalone CI"
fi

cmake -S "${WHILLATS_DIR}" -B "${WHILLATS_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=OFF \
  -DWHISPER_CUDA=OFF \
  -DGGML_CUBLAS=OFF \
  -DLLAMA_CUDA=OFF
cmake --build "${WHILLATS_BUILD_DIR}" --config Release --parallel 4

# Normalize output paths expected by directcall GN files.
if [ -d "${WHILLATS_BUILD_DIR}/lib/Release" ]; then
  mkdir -p "${WHILLATS_BUILD_DIR}/lib/release"
  cp -a "${WHILLATS_BUILD_DIR}/lib/Release/." "${WHILLATS_BUILD_DIR}/lib/release/" || true
fi
if [ -d "${WHILLATS_BUILD_DIR}/bin/Release" ]; then
  cp -a "${WHILLATS_BUILD_DIR}/bin/Release/." "${WHILLATS_BUILD_DIR}/bin/" || true
fi

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

"${GN_BIN}" gen "${OUT_DIR}" --args='target_os="linux" is_debug=false is_clang=false use_sysroot=false treat_warnings_as_errors=false rtc_include_opus=true rtc_include_tests=true rtc_build_examples=true rtc_build_sdk=false rtc_enable_symbol_export=true rtc_use_speech_audio_devices=true use_custom_libcxx=false enable_js_protobuf=false rtc_enable_protobuf=false enable_libaom=false'
if [ -x "/usr/bin/ninja" ]; then
  NINJA_BIN="/usr/bin/ninja"
elif command -v ninja-build >/dev/null 2>&1; then
  NINJA_BIN="$(command -v ninja-build)"
elif command -v ninja >/dev/null 2>&1; then
  NINJA_BIN="$(command -v ninja)"
else
  echo "[build] ninja binary not found"
  exit 1
fi
"${NINJA_BIN}" -C "${OUT_DIR}" directcall

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/lib"
cp "${OUT_DIR}/directcall" "${DIST_DIR}/directcall"
if [ -f "${OUT_DIR}/libdirect.so" ]; then
  cp "${OUT_DIR}/libdirect.so" "${DIST_DIR}/libdirect.so"
  cp "${OUT_DIR}/libdirect.so" "${DIST_DIR}/lib/" || true
fi

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
