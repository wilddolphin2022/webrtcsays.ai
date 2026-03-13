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
  build-essential \
  ca-certificates \
  clang \
  cmake \
  git \
  libasound2-dev \
  libgtk-3-dev \
  libxtst6 \
  lld \
  ninja-build \
  pkg-config \
  python3 \
  rsync \
  unzip

if [ ! -d "${HOME}/depot_tools" ]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "${HOME}/depot_tools"
fi
export PATH="${HOME}/depot_tools:${PATH}"

cd "${ROOT_DIR}"
if [ -f ".gitmodules" ]; then
  git submodule update --init --recursive modules/third_party/whillats
fi

if [ -x "build/install-build-deps.sh" ]; then
  ./build/install-build-deps.sh --no-chromeos-fonts || true
fi

python3 tools/clang/scripts/update.py

cmake -S "${WHILLATS_DIR}" -B "${WHILLATS_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=OFF \
  -DWHISPER_CUDA=OFF \
  -DGGML_CUBLAS=OFF \
  -DLLAMA_CUDA=OFF
cmake --build "${WHILLATS_BUILD_DIR}" --config Release --parallel 4

gn gen "${OUT_DIR}" --args='target_os="linux" is_debug=false rtc_include_opus=true rtc_build_examples=true rtc_enable_symbol_export=true rtc_use_speech_audio_devices=true use_custom_libcxx=false'
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
