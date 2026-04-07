#!/usr/bin/env bash
# deploy-talkingface5.sh
# Deploy talkingface5: whillats_server (Whisper + embedded Llama + Piper)
# to a clean Ubuntu/Debian x86_64 machine.
#
# Usage:
#   ./deploy-talkingface5.sh <user@host> [ssh-key]
#   HF_TOKEN=hf_xxx ./deploy-talkingface5.sh root@host ~/.ssh/id_wd2025
#
# Architecture:
#   - whillats_server — Whisper STT + embedded Llama (llama.cpp) + Piper TTS
#   - directcall (libdirect.so) — WebRTC signaling, audio pipeline
#   - demo.html      — browser UI (deployed via FTP to www.wilddolphin.us)

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
BRANCH="talkingface5"
WEBRTCSAYS_REPO="https://github.com/wilddolphin2025/webrtcsays.ai.git"
WHILLATS_REPO="https://github.com/wilddolphin2025/whillats.git"

DEPLOY_DIR="/opt/directcall3-dev"
WHILLATS_DIR="/opt/whillats3"
MODELS_DIR="/opt/models"
BUILD_DIR="/opt/webrtcsays-build"
HF_PIPER_BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main"

# Models
WHISPER_MODEL="ggml-small.bin"
LLAMA_MODEL="google_gemma-4-E4B-it-Q4_K_M.gguf"
LLAMA_MMPROJ="mmproj-BF16.gguf"

# Piper voice models (paths relative to HF_PIPER_BASE)
PIPER_EN="en/en_US/lessac/low/en_US-lessac-low.onnx"
PIPER_ES="es/es_ES/carlfm/x_low/es_ES-carlfm-x_low.onnx"
PIPER_RU="ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx"
PIPER_ZH="zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx"

# GN build args
GN_ARGS='target_os="linux" is_debug=false is_clang=true use_sysroot=false
  treat_warnings_as_errors=false rtc_include_opus=true rtc_include_tests=false
  rtc_build_examples=true rtc_build_sdk=false rtc_enable_symbol_export=true
  rtc_use_speech_audio_devices=true use_custom_libcxx=true
  enable_js_protobuf=false rtc_enable_protobuf=false enable_libaom=false'

# ---------------------------------------------------------------------------
# SSH target
# ---------------------------------------------------------------------------
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <user@host> [ssh-key-path]"
    exit 1
fi
SSH_TARGET="$1"
SSH_KEY="${2:-}"
SSH_OPTS="-o StrictHostKeyChecking=accept-new"
[[ -n "$SSH_KEY" ]] && SSH_OPTS="$SSH_OPTS -i $SSH_KEY"
SSH="ssh $SSH_OPTS $SSH_TARGET"

echo "=== talkingface5 deploy to $SSH_TARGET ==="

# ---------------------------------------------------------------------------
# Phase 1: system dependencies
# ---------------------------------------------------------------------------
echo "[1/8] Installing system dependencies..."
$SSH << 'EOF'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    git build-essential cmake ninja-build pkg-config python3 python3-pip \
    libpulse-dev libasound2-dev curl wget ca-certificates lsb-release \
    autoconf automake libtool
if [ ! -d "$HOME/depot_tools" ]; then
    git clone --depth=1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$HOME/depot_tools"
fi
export PATH="$HOME/depot_tools:$PATH"
echo "export PATH=\"\$HOME/depot_tools:\$PATH\"" >> ~/.bashrc || true
pip3 install -q huggingface_hub 2>/dev/null || true
EOF

# ---------------------------------------------------------------------------
# Phase 2: build whillats_server (talkingface5) — Whisper + embedded Llama + Piper
# ---------------------------------------------------------------------------
echo "[2/8] Building whillats_server (branch: $BRANCH)..."
$SSH << EOF
set -euo pipefail
BRANCH="$BRANCH"
WHILLATS_REPO="$WHILLATS_REPO"
WHILLATS_DIR="$WHILLATS_DIR"

if [ -d "\$WHILLATS_DIR/.git" ]; then
    cd "\$WHILLATS_DIR"
    git fetch origin "\$BRANCH"
    git checkout "\$BRANCH"
    git pull origin "\$BRANCH"
else
    git clone --branch "\$BRANCH" --recursive "\$WHILLATS_REPO" "\$WHILLATS_DIR"
    cd "\$WHILLATS_DIR"
fi

rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_METAL=OFF \
    -DGGML_CUDA=OFF \
    -DWHILLATS_PIPER=ON
cmake --build build --config Release -j\$(nproc) --target whillats_server
echo "whillats_server built: \$(ls -lh build/bin/Release/whillats_server)"

mkdir -p "$DEPLOY_DIR/lib"
BIN="\$WHILLATS_DIR/build/bin/Release"
cp "\$BIN/whillats_server" "$DEPLOY_DIR/whillats_server.talkingface5"
cp -r "\$BIN/espeak-ng-data" "$DEPLOY_DIR/espeak-ng-data.talkingface5" 2>/dev/null || true
for lib in \$WHILLATS_DIR/build/lib/*.so* \$WHILLATS_DIR/build/lib/Release/*.so*; do
    [ -f "\$lib" ] && cp "\$lib" "$DEPLOY_DIR/lib/" 2>/dev/null || true
done
echo "whillats_server staged."
EOF

# ---------------------------------------------------------------------------
# Phase 4: build webrtcsays.ai → directcall + libdirect.so
# ---------------------------------------------------------------------------
echo "[4/9] Building webrtcsays.ai (branch: $BRANCH)..."
$SSH << EOF
set -euo pipefail
export PATH="\$HOME/depot_tools:\$PATH"
BRANCH="$BRANCH"
BUILD_DIR="$BUILD_DIR"
GN_ARGS='$GN_ARGS'

if [ -d "\$BUILD_DIR/src/.git" ]; then
    cd "\$BUILD_DIR/src"
    git fetch origin "\$BRANCH" && git checkout "\$BRANCH" && git pull origin "\$BRANCH"
    cd modules/third_party/whillats
    git fetch origin "\$BRANCH" && git checkout "\$BRANCH" && git pull origin "\$BRANCH"
    cd "\$BUILD_DIR/src"
else
    mkdir -p "\$BUILD_DIR" && cd "\$BUILD_DIR"
    cat > .gclient << 'GCLIENT'
solutions = [
  {
    "managed": False,
    "name": "src",
    "url": "$WEBRTCSAYS_REPO@$BRANCH",
    "custom_deps": {},
    "deps_file": "DEPS",
  },
]
GCLIENT
    gclient sync --nohooks --no-history --shallow
    cd src && git checkout "\$BRANCH"
fi

python3 tools/clang/scripts/update.py || true
gn gen out/release --args="\$GN_ARGS"
ninja -C out/release libdirect.so directcall

cp out/release/libdirect.so "$DEPLOY_DIR/lib/libdirect.so"
cp out/release/directcall   "$DEPLOY_DIR/directcall"
echo "Build done: \$(ls -lh $DEPLOY_DIR/lib/libdirect.so $DEPLOY_DIR/directcall)"
EOF

# ---------------------------------------------------------------------------
# Phase 5: download AI models
# ---------------------------------------------------------------------------
echo "[5/9] Downloading AI models..."
$SSH << EOF
set -euo pipefail
MODELS_DIR="$MODELS_DIR"
HF_TOKEN="\${HF_TOKEN:-}"
mkdir -p "\$MODELS_DIR/piper"

# Whisper small
if [ ! -f "\$MODELS_DIR/ggml-small.bin" ]; then
    echo "  Downloading Whisper small..."
    wget -q --show-progress -O "\$MODELS_DIR/ggml-small.bin" \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin"
fi

# Gemma4 Q2_K (gated)
if [ ! -f "\$MODELS_DIR/$LLAMA_MODEL" ]; then
    if [ -z "\$HF_TOKEN" ]; then
        echo "  WARNING: HF_TOKEN not set — Gemma4 model skipped. Copy manually to \$MODELS_DIR/$LLAMA_MODEL"
    else
        echo "  Downloading Gemma4 Q2_K (~4.5 GB)..."
        python3 -c "
from huggingface_hub import hf_hub_download; import os
hf_hub_download(repo_id='bartowski/google_gemma-4-E4B-it-GGUF',
    filename='$LLAMA_MODEL', local_dir='\$MODELS_DIR', token=os.environ['HF_TOKEN'])
"
    fi
fi

# Gemma4 mmproj
if [ ! -f "\$MODELS_DIR/$LLAMA_MMPROJ" ]; then
    if [ -n "\${HF_TOKEN:-}" ]; then
        echo "  Downloading mmproj (~950 MB)..."
        python3 -c "
from huggingface_hub import hf_hub_download; import os
hf_hub_download(repo_id='ggml-org/gemma-4-E4B-it-GGUF',
    filename='mmproj-gemma-4-e4b-it-f16.gguf',
    local_dir='\$MODELS_DIR', local_filename='$LLAMA_MMPROJ',
    token=os.environ['HF_TOKEN'])
"
    fi
fi

# Piper voices
HF_PIPER_BASE="$HF_PIPER_BASE"
dl() { local lang="\$1" path="\$2" name="\$(basename \$2)"
    [ -f "\$MODELS_DIR/piper/\$name" ] && return
    echo "  Piper [\$lang]: \$name"
    wget -q "\${HF_PIPER_BASE}/\${path}" -O "\$MODELS_DIR/piper/\$name"
    wget -q "\${HF_PIPER_BASE}/\${path}.json" -O "\$MODELS_DIR/piper/\$name.json"
}
dl en "en/en_US/lessac/low/en_US-lessac-low.onnx"
dl es "es/es_ES/carlfm/x_low/es_ES-carlfm-x_low.onnx"
dl ru "ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx"
dl zh "zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx"
echo "  Models ready."
EOF

# ---------------------------------------------------------------------------
# Phase 6: TLS certs
# ---------------------------------------------------------------------------
echo "[6/9] TLS certificates..."
$SSH << EOF
set -euo pipefail
if [ ! -f "$DEPLOY_DIR/cert.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$DEPLOY_DIR/key.pem" -out "$DEPLOY_DIR/cert.pem" \
        -subj "/CN=\$(hostname -f)" \
        -addext "subjectAltName=IP:\$(curl -s ifconfig.me 2>/dev/null || hostname -I | awk '{print \$1}')"
fi
EOF

# ---------------------------------------------------------------------------
# Phase 7: runtime scripts + config
# ---------------------------------------------------------------------------
echo "[7/8] Writing runtime files..."

$SSH << 'EOF'
cat > /opt/directcall3-dev/run-directcall.sh << 'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${SELF_DIR}/lib:${LD_LIBRARY_PATH:-}"
exec stdbuf -oL "${SELF_DIR}/directcall" "$@"
SCRIPT
chmod +x /opt/directcall3-dev/run-directcall.sh
EOF

$SSH << EOF
cat > "$DEPLOY_DIR/config.talkingface5.json" << 'CONFIG'
{
  "mode": "callee",
  "address": "0.0.0.0:3458",
  "user_name": "Slim",
  "room_name": "room101",
  "websocket_signaling": true,
  "websocket_port": 3459,
  "encryption": true,
  "whisper": true,
  "llama": true,
  "video": true,
  "bonjour": false,
  "language": "auto",
  "whisper_model": "$MODELS_DIR/ggml-small.bin",
  "llama_model":   "$MODELS_DIR/$LLAMA_MODEL",
  "llama_mmproj":  "$MODELS_DIR/$LLAMA_MMPROJ",
  "webrtc_cert_path": "$DEPLOY_DIR/cert.pem",
  "webrtc_key_path":  "$DEPLOY_DIR/key.pem",
  "webrtc_speech_initial_playout_wav": "",
  "turns": [
    ["turn:turn.wilddolphin.us:3478?transport=udp", "webrtcsays.ai", "wilddolphin"],
    ["turn:turn.wilddolphin.us:5349?transport=tcp", "webrtcsays.ai", "wilddolphin"]
  ],
  "talking_face": "$DEPLOY_DIR/RobotPhoneLogo.jpeg",
  "whisper_threads": 4,
  "llama_threads": 0,
  "tts_threads": 2
}
CONFIG
EOF

# ---------------------------------------------------------------------------
# Phase 8: systemd service (directcall)
# ---------------------------------------------------------------------------
echo "[8/8] Writing systemd service..."

$SSH << EOF
# directcall service
cat > /etc/systemd/system/directcall3-talkingface5.service << 'UNIT'
[Unit]
Description=DirectCall3 talkingface5 (WebRTC + Whisper + embedded Llama + Piper)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$DEPLOY_DIR
ExecStart=$DEPLOY_DIR/run-directcall.sh --config $DEPLOY_DIR/config.talkingface5.json
Restart=on-failure
RestartSec=5
Environment=LD_LIBRARY_PATH=$DEPLOY_DIR/lib
Environment=WHILLATS_SERVER=$DEPLOY_DIR/whillats_server.talkingface5
Environment=WHISPER_MODEL=$MODELS_DIR/ggml-small.bin
Environment=PIPER_MODEL=$MODELS_DIR/piper/en_US-lessac-low.onnx
Environment=PIPER_MODEL_ES=$MODELS_DIR/piper/es_ES-carlfm-x_low.onnx
Environment=PIPER_MODEL_RU=$MODELS_DIR/piper/ru_RU-ruslan-medium.onnx
Environment=PIPER_MODEL_ZH=$MODELS_DIR/piper/zh_CN-huayan-medium.onnx
Environment=ESPEAK_DATA_PATH=$DEPLOY_DIR/espeak-ng-data.talkingface5

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable directcall3-talkingface5
systemctl restart directcall3-talkingface5
sleep 3
systemctl is-active directcall3-talkingface5
EOF

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Phase 8 cont: deploy demo.html via FTP
# ---------------------------------------------------------------------------
echo "  Deploying demo.html to www.wilddolphin.us..."
DEMO_SRC="$(dirname "$0")/src/modules/third_party/whillats/demo.html"
if [ -f "$DEMO_SRC" ]; then
    curl -s --ftp-create-dirs -T "$DEMO_SRC" \
        "ftp://www.wilddolphin.us/www/demo.html" \
        --user "wilddolphin:WildAt2026!" && echo "  demo.html uploaded to FTP"
else
    echo "  WARNING: demo.html not found at $DEMO_SRC"
fi

echo ""
echo "=== talkingface5 deploy complete ==="
echo ""
echo "Service logs:"
echo "  directcall: journalctl -u directcall3-talkingface5 -f"
echo ""
echo "WebRTC signaling: ws://<host>:3459"
echo "demo.html:        https://www.wilddolphin.us/demo.html"
