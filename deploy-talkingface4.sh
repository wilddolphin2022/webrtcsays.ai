#!/usr/bin/env bash
# deploy-talkingface4.sh
# Full deploy of talkingface4 (Gemma-4 multimodal, multilingual STT/TTS)
# to a clean Ubuntu/Debian x86_64 machine.
#
# Usage:
#   ./deploy-talkingface4.sh <ssh-host> [ssh-key]
#
# Examples:
#   ./deploy-talkingface4.sh root@rtc.wilddolphin.us
#   ./deploy-talkingface4.sh root@1.2.3.4 ~/.ssh/id_wd2025
#
# What this script does (all on the remote):
#   1. Install system dependencies
#   2. Clone/build whillats (talkingface4 branch) → whillats_server binary
#   3. Clone/build webrtcsays.ai src (talkingface4) → directcall + libdirect.so
#   4. Download AI models (Whisper, Gemma4 Q2_K + mmproj, Piper voices)
#   5. Install TLS certs (self-signed if not present)
#   6. Write /opt deployment layout
#   7. Write + enable systemd service directcall3-talkingface4

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
BRANCH="talkingface4"
WEBRTCSAYS_REPO="https://github.com/wilddolphin2025/webrtcsays.ai.git"
WHILLATS_REPO="https://github.com/wilddolphin2025/whillats.git"

DEPLOY_DIR="/opt/directcall3-dev"
WHILLATS_DIR="/opt/whillats3"
MODELS_DIR="/opt/models"
BUILD_DIR="/opt/webrtcsays-build"
HF_PIPER_BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main"

# Model files
WHISPER_MODEL="ggml-small.bin"
LLAMA_MODEL="google_gemma-4-E4B-it-Q2_K.gguf"
LLAMA_MMPROJ="mmproj-BF16.gguf"

# Piper voice models
declare -A PIPER_VOICES=(
    ["en"]="en/en_US/lessac/low/en_US-lessac-low.onnx"
    ["es"]="es/es_ES/carlfm/x_low/es_ES-carlfm-x_low.onnx"
    ["ru"]="ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx"
    ["zh"]="zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx"
)

# GN build args for libdirect.so
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
SCP="scp $SSH_OPTS"

echo "=== talkingface4 deploy to $SSH_TARGET ==="

# ---------------------------------------------------------------------------
# Helper: run on remote
# ---------------------------------------------------------------------------
remote() { $SSH -- bash -s <<'REMOTE_EOF'
'"$*"'
REMOTE_EOF
}

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
# depot_tools for gn/ninja (WebRTC build)
if [ ! -d "$HOME/depot_tools" ]; then
    git clone --depth=1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$HOME/depot_tools"
fi
export PATH="$HOME/depot_tools:$PATH"
echo "export PATH=\"\$HOME/depot_tools:\$PATH\"" >> ~/.bashrc || true
pip3 install -q huggingface_hub 2>/dev/null || true
EOF

# ---------------------------------------------------------------------------
# Phase 2: build whillats (talkingface4) → whillats_server
# ---------------------------------------------------------------------------
echo "[2/8] Building whillats (branch: $BRANCH)..."
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
    mkdir -p "\$(dirname \$WHILLATS_DIR)"
    git clone --branch "\$BRANCH" --recursive "\$WHILLATS_REPO" "\$WHILLATS_DIR"
    cd "\$WHILLATS_DIR"
fi

# Build with Piper TTS, CPU-only
rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_METAL=OFF \
    -DGGML_CUDA=OFF \
    -DWHILLATS_PIPER=ON
cmake --build build --config Release -j\$(nproc) --target whillats_server
echo "whillats_server built: \$(ls -lh build/bin/Release/whillats_server)"

# Stage for deployment
mkdir -p "$DEPLOY_DIR/lib"
BIN="\$WHILLATS_DIR/build/bin/Release"
# Copy server binary (versioned)
cp "\$BIN/whillats_server" "$DEPLOY_DIR/whillats_server.talkingface4"
# Copy espeak-ng data
cp -r "\$BIN/espeak-ng-data" "$DEPLOY_DIR/espeak-ng-data.talkingface4" 2>/dev/null || true
# Copy ggml/llama shared libs needed at runtime
for lib in \$WHILLATS_DIR/build/lib*.so* \$WHILLATS_DIR/build/bin/Release/lib*.so*; do
    [ -f "\$lib" ] && cp "\$lib" "$DEPLOY_DIR/lib/" 2>/dev/null || true
done
echo "whillats stage done."
EOF

# ---------------------------------------------------------------------------
# Phase 3: build webrtcsays.ai src → directcall + libdirect.so
# ---------------------------------------------------------------------------
echo "[3/8] Building webrtcsays.ai (branch: $BRANCH)..."
$SSH << EOF
set -euo pipefail
export PATH="\$HOME/depot_tools:\$PATH"
BRANCH="$BRANCH"
BUILD_DIR="$BUILD_DIR"
GN_ARGS='$GN_ARGS'

if [ -d "\$BUILD_DIR/src/.git" ]; then
    cd "\$BUILD_DIR/src"
    git fetch origin "\$BRANCH"
    git checkout "\$BRANCH"
    git pull origin "\$BRANCH"
    # Update whillats submodule to talkingface4
    cd modules/third_party/whillats
    git fetch origin "\$BRANCH"
    git checkout "\$BRANCH"
    git pull origin "\$BRANCH"
    cd "\$BUILD_DIR/src"
else
    mkdir -p "\$BUILD_DIR"
    cd "\$BUILD_DIR"
    # Create .gclient file
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
    cd src
    git checkout "\$BRANCH"
fi

# Install sysroot / clang toolchain
python3 tools/clang/scripts/update.py || true

# Configure + build
gn gen out/release --args="\$GN_ARGS"
ninja -C out/release libdirect.so directcall

echo "Build done:"
ls -lh out/release/libdirect.so out/release/directcall

# Stage
cp out/release/libdirect.so "$DEPLOY_DIR/lib/libdirect.so"
cp out/release/directcall   "$DEPLOY_DIR/directcall"
echo "Stage done."
EOF

# ---------------------------------------------------------------------------
# Phase 4: download AI models
# ---------------------------------------------------------------------------
echo "[4/8] Downloading AI models..."
$SSH << EOF
set -euo pipefail
MODELS_DIR="$MODELS_DIR"
HF_TOKEN="\${HF_TOKEN:-}"
mkdir -p "\$MODELS_DIR/piper"

# --- Whisper small ---
WHISPER_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin"
if [ ! -f "\$MODELS_DIR/ggml-small.bin" ]; then
    echo "  Downloading Whisper small (~466 MB)..."
    wget -q --show-progress -O "\$MODELS_DIR/ggml-small.bin" "\$WHISPER_URL"
fi

# --- Gemma4 Q2_K + mmproj (requires HF token / gated) ---
if [ ! -f "\$MODELS_DIR/$LLAMA_MODEL" ]; then
    if [ -z "\$HF_TOKEN" ]; then
        echo "  WARNING: HF_TOKEN not set. Cannot download gated Gemma4 model."
        echo "  Set HF_TOKEN and re-run, or copy the file manually to \$MODELS_DIR/$LLAMA_MODEL"
    else
        echo "  Downloading Gemma4 Q2_K (~4.5 GB)..."
        python3 -c "
from huggingface_hub import hf_hub_download
import os
path = hf_hub_download(
    repo_id='bartowski/google_gemma-4-E4B-it-GGUF',
    filename='$LLAMA_MODEL',
    local_dir='\$MODELS_DIR',
    token=os.environ['HF_TOKEN']
)
print('Downloaded:', path)
"
    fi
fi

if [ ! -f "\$MODELS_DIR/$LLAMA_MMPROJ" ]; then
    if [ -z "\$HF_TOKEN" ]; then
        echo "  WARNING: HF_TOKEN not set. Cannot download mmproj."
    else
        echo "  Downloading mmproj (~950 MB)..."
        python3 -c "
from huggingface_hub import hf_hub_download
import os
# Try original source first, then bartowski mirror
for repo in ['ggml-org/gemma-4-E4B-it-GGUF']:
    try:
        path = hf_hub_download(repo_id=repo, filename='mmproj-gemma-4-e4b-it-f16.gguf',
            local_dir='\$MODELS_DIR', local_filename='$LLAMA_MMPROJ',
            token=os.environ['HF_TOKEN'])
        print('Downloaded:', path)
        break
    except Exception as e:
        print('Trying next repo:', e)
"
    fi
fi

# --- Piper TTS voice models ---
echo "  Downloading Piper voice models..."
PIPER_BASE="$HF_PIPER_BASE"
download_piper() {
    local lang="\$1" path="\$2"
    local name="\$(basename \$path)"
    local dest="\$MODELS_DIR/piper/\$name"
    if [ ! -f "\$dest" ]; then
        echo "    Piper \$lang: \$name"
        wget -q "\${PIPER_BASE}/\${path}" -O "\$dest"
        wget -q "\${PIPER_BASE}/\${path}.json" -O "\$dest.json"
    fi
}
download_piper en "en/en_US/lessac/low/en_US-lessac-low.onnx"
download_piper es "es/es_ES/carlfm/x_low/es_ES-carlfm-x_low.onnx"
download_piper ru "ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx"
download_piper zh "zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx"

echo "  Models ready:"
ls -lh "\$MODELS_DIR"/*.{bin,gguf} 2>/dev/null || true
ls -lh "\$MODELS_DIR/piper/"*.onnx 2>/dev/null | awk '{print "   ", \$5, \$9}'
EOF

# ---------------------------------------------------------------------------
# Phase 5: TLS certs
# ---------------------------------------------------------------------------
echo "[5/8] TLS certificates..."
$SSH << EOF
set -euo pipefail
DEPLOY_DIR="$DEPLOY_DIR"
if [ ! -f "\$DEPLOY_DIR/cert.pem" ] || [ ! -f "\$DEPLOY_DIR/key.pem" ]; then
    echo "  Generating self-signed cert (10 years)..."
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "\$DEPLOY_DIR/key.pem" -out "\$DEPLOY_DIR/cert.pem" \
        -subj "/CN=\$(hostname -f)" \
        -addext "subjectAltName=IP:\$(curl -s ifconfig.me 2>/dev/null || hostname -I | awk '{print \$1}')"
else
    echo "  Certs already present."
fi
EOF

# ---------------------------------------------------------------------------
# Phase 6: talking face image
# ---------------------------------------------------------------------------
echo "[6/8] Assets..."
$SSH << EOF
DEPLOY_DIR="$DEPLOY_DIR"
if [ ! -f "\$DEPLOY_DIR/RobotPhoneLogo.jpeg" ]; then
    # Copy from build if available, else create placeholder
    cp "$BUILD_DIR/src/RobotPhoneLogo.jpeg" "\$DEPLOY_DIR/" 2>/dev/null || \
    cp "$WHILLATS_DIR/media/RobotPhoneLogo.jpeg" "\$DEPLOY_DIR/" 2>/dev/null || \
    echo "  WARNING: RobotPhoneLogo.jpeg not found — set talking_face in config."
fi
EOF

# ---------------------------------------------------------------------------
# Phase 7: write runtime files
# ---------------------------------------------------------------------------
echo "[7/8] Writing runtime files..."

# run-directcall.sh
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

# config.talkingface4.json
$SSH << EOF
cat > "$DEPLOY_DIR/config.talkingface4.json" << 'CONFIG'
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
  "language": "en",
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
  "llama_threads": 6,
  "tts_threads": 2,
  "whispers": {
    "en": ["Hello, how are you?"]
  }
}
CONFIG
EOF

# systemd service
$SSH << EOF
cat > /etc/systemd/system/directcall3-talkingface4.service << 'UNIT'
[Unit]
Description=DirectCall3 talkingface4 (Gemma-4 multimodal, multilingual TTS)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$DEPLOY_DIR
ExecStart=$DEPLOY_DIR/run-directcall.sh --config $DEPLOY_DIR/config.talkingface4.json
Restart=on-failure
RestartSec=5
Environment=LD_LIBRARY_PATH=$DEPLOY_DIR/lib
Environment=WHILLATS_SERVER=$DEPLOY_DIR/whillats_server.talkingface4
Environment=WHISPER_MODEL=$MODELS_DIR/ggml-small.bin
Environment=LLAMA_MODEL=$MODELS_DIR/$LLAMA_MODEL
Environment=LLAMA_MMPROJ=$MODELS_DIR/$LLAMA_MMPROJ
Environment=PIPER_MODEL=$MODELS_DIR/piper/en_US-lessac-low.onnx
Environment=PIPER_MODEL_ES=$MODELS_DIR/piper/es_ES-carlfm-x_low.onnx
Environment=PIPER_MODEL_RU=$MODELS_DIR/piper/ru_RU-ruslan-medium.onnx
Environment=PIPER_MODEL_ZH=$MODELS_DIR/piper/zh_CN-huayan-medium.onnx
Environment=ESPEAK_DATA_PATH=$DEPLOY_DIR/espeak-ng-data.talkingface4

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable directcall3-talkingface4
systemctl restart directcall3-talkingface4
sleep 3
systemctl is-active directcall3-talkingface4
EOF

# ---------------------------------------------------------------------------
# Phase 8: deploy demo.html
# ---------------------------------------------------------------------------
echo "[8/8] Deploying demo.html..."
$SCP "$WHILLATS_REPO_LOCAL_PATH/demo.html" "$SSH_TARGET:/tmp/demo.html" 2>/dev/null || \
$SSH << 'EOF'
# If local copy unavailable, pull from the built whillats submodule
cp "$BUILD_DIR/src/modules/third_party/whillats/demo.html" /tmp/demo.html 2>/dev/null || \
    echo "  WARNING: demo.html not copied — serve manually from the whillats repo"
EOF

echo ""
echo "=== Deploy complete ==="
echo "Service: directcall3-talkingface4"
echo "WebSocket (browser): ws://<host>:3459"
echo "demo.html: /tmp/demo.html"
echo ""
echo "To check logs:  ssh $SSH_TARGET journalctl -u directcall3-talkingface4 -f"
echo "To roll back model: edit $DEPLOY_DIR/config.talkingface4.json llama_model"
