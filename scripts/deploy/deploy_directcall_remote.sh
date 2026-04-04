#!/usr/bin/env bash
set -euo pipefail

# === Configurable defaults ===
DEPLOY_HOST="${DEPLOY_HOST:-217.77.3.65}"
DEPLOY_USER="${DEPLOY_USER:-root}"
DEPLOY_PORT="${DEPLOY_PORT:-22}"
DEPLOY_PATH="${DEPLOY_PATH:-/opt/directcall}"
SERVICE_NAME="${SERVICE_NAME:-directcall}"
SSH_KEY_PATH="${SSH_KEY_PATH:-${HOME}/.ssh/id_wd2025}"
SSH_KEY_PATH="${SSH_KEY_PATH/#\~/$HOME}"
ENABLE_SIGNAL_BRIDGE="${ENABLE_SIGNAL_BRIDGE:-false}"
BRIDGE_SERVICE_NAME="${BRIDGE_SERVICE_NAME:-directcall-bridge}"
SIGNAL_BASE_URL="${SIGNAL_BASE_URL:-https://www.wilddolphin.us/signal.php}"
BRIDGE_ROOM="${BRIDGE_ROOM:-testroom}"
CALLEE_HOST="${CALLEE_HOST:-127.0.0.1}"
CALLEE_PORT="${CALLEE_PORT:-3456}"
CALLEE_ROOM="${CALLEE_ROOM:-room101}"
MODELS_PATH="${MODELS_PATH:-/opt/models}"
HF_TOKEN="${HF_TOKEN:-}"
SKIP_COTURN="${SKIP_COTURN:-true}"
WHILLATS_BRANCH="${WHILLATS_BRANCH:-talkingface3}"
WHILLATS_DIR_REMOTE="/opt/whillats3"
WEBRTC_BRANCH="${WEBRTC_BRANCH:-talkingface}"
WEBRTC_DIR_REMOTE="/opt/webrtcsays-build"
WHISPER_MODEL_URL="${WHISPER_MODEL_URL:-https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin}"
WHISPER_MODEL_FILE="${WHISPER_MODEL_FILE:-ggml-base.bin}"
LLAMA_MODEL_URL="${LLAMA_MODEL_URL:-https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf}"
LLAMA_MODEL_FILE="${LLAMA_MODEL_FILE:-Qwen2.5-1.5B-Instruct-Q4_K_M.gguf}"
PIPER_MODEL_URL="${PIPER_MODEL_URL:-https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/low/en_US-lessac-low.onnx}"
PIPER_MODEL_JSON_URL="${PIPER_MODEL_JSON_URL:-https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/low/en_US-lessac-low.onnx.json}"
PIPER_MODEL_FILE="${PIPER_MODEL_FILE:-en_US-lessac-low.onnx}"
DEPLOY_DRY_RUN="${DEPLOY_DRY_RUN:-false}"
CONFIG_PATH="${1:-}"

deploy_dry_run() {
  case "${DEPLOY_DRY_RUN}" in
    true|1|yes|TRUE|YES) return 0 ;;
    *) return 1 ;;
  esac
}

if [ ! -f "${SSH_KEY_PATH}" ]; then
  echo "SSH key not found: ${SSH_KEY_PATH}"
  exit 1
fi

if deploy_dry_run; then
  echo "[deploy] DRY RUN mode — would clone, build, and configure on ${DEPLOY_USER}@${DEPLOY_HOST}"
  echo "[deploy] WebRTC repo: https://github.com/wilddolphin2025/webrtcsays.ai.git branch ${WEBRTC_BRANCH}"
  echo "[deploy] Whillats repo: https://github.com/wilddolphin2025/whillats.git branch ${WHILLATS_BRANCH}"
  echo "[deploy] Deploy path: ${DEPLOY_PATH}"
  exit 0
fi

SSH_OPTS=(-i "${SSH_KEY_PATH}" -p "${DEPLOY_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
REMOTE="${DEPLOY_USER}@${DEPLOY_HOST}"

run_remote() {
  if [ "${DEPLOY_USER}" = "root" ]; then
    ssh "${SSH_OPTS[@]}" "${REMOTE}" "$1"
  else
    ssh "${SSH_OPTS[@]}" "${REMOTE}" "sudo bash -c '$1'"
  fi
}

echo "[deploy] === Phase 1: System packages ==="
run_remote "apt-get update -qq && apt-get install -y -qq build-essential cmake git autoconf automake libtool pkg-config libasound2-dev libgomp1 libgtk-3-dev libxtst6 espeak-ng espeak-ng-data libclang-dev clang lld ninja-build python3 python3-pip curl unzip rsync nginx 2>/dev/null || true"

echo "[deploy] === Phase 2: depot_tools ==="
run_remote "if [ ! -d /root/depot_tools ]; then git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git /root/depot_tools; fi && /root/depot_tools/ensure_bootstrap 2>/dev/null || true"

echo "[deploy] === Phase 3: Clone whillats and build whillats_server ==="
run_remote "mkdir -p /usr/local/share && ESPEAK_SRC=\$(find /usr/lib -name espeak-ng-data -type d 2>/dev/null | head -1); [ -n \"\${ESPEAK_SRC}\" ] && ln -sf \"\${ESPEAK_SRC}\" /usr/local/share/espeak-ng-data 2>/dev/null || true; if [ ! -d '${WHILLATS_DIR_REMOTE}/.git' ]; then rm -rf '${WHILLATS_DIR_REMOTE}'; git clone --branch '${WHILLATS_BRANCH}' --recursive https://github.com/wilddolphin2025/whillats.git '${WHILLATS_DIR_REMOTE}'; else cd '${WHILLATS_DIR_REMOTE}' && git fetch origin '${WHILLATS_BRANCH}' && git checkout -B '${WHILLATS_BRANCH}' 'origin/${WHILLATS_BRANCH}' && git submodule update --init --recursive; fi && cd '${WHILLATS_DIR_REMOTE}' && CUDA_FLAG='OFF'; if command -v nvcc >/dev/null 2>&1; then CUDA_FLAG='ON'; echo 'CUDA detected'; fi && rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release -DWHILLATS_PIPER=ON -DWHILLATS_OLD_ABI=ON -DGGML_CUDA=\${CUDA_FLAG} 2>&1 | tail -5 && cmake --build build --config Release --parallel \$(nproc) --target whillats_server 2>&1 | tail -10 && echo 'whillats_server built' && find build -name whillats_server -type f 2>/dev/null | head -1"

echo "[deploy] === Phase 4: Clone webrtcsays.ai and build directcall ==="
run_remote "if [ ! -d '${WEBRTC_DIR_REMOTE}/src/.git' ]; then rm -rf '${WEBRTC_DIR_REMOTE}'; git clone --branch '${WEBRTC_BRANCH}' https://github.com/wilddolphin2025/webrtcsays.ai.git '${WEBRTC_DIR_REMOTE}/src'; cd '${WEBRTC_DIR_REMOTE}/src' && git submodule update --init --recursive modules/third_party/whillats; else cd '${WEBRTC_DIR_REMOTE}/src' && git fetch origin '${WEBRTC_BRANCH}' && git checkout -B '${WEBRTC_BRANCH}' 'origin/${WEBRTC_BRANCH}' && git submodule update --init --recursive modules/third_party/whillats; fi"

echo "[deploy] === Phase 5: Build directcall on remote ==="
run_remote "export PATH=/root/depot_tools:\$PATH && cd '${WEBRTC_DIR_REMOTE}/src' && rm -rf out/release && gn gen out/release --args='target_os=\"linux\" is_debug=false is_clang=true use_sysroot=false treat_warnings_as_errors=false rtc_include_opus=true rtc_include_tests=false rtc_build_examples=true rtc_build_sdk=false rtc_enable_symbol_export=true rtc_use_speech_audio_devices=true use_custom_libcxx=true enable_js_protobuf=false rtc_enable_protobuf=false enable_libaom=false' 2>&1 | tail -5 && autoninja -C out/release directcall 2>&1 | tail -5"

echo "[deploy] === Phase 6: Locate whillats_server and espeak data ==="
WHILLATS_SERVER_PATH=$(ssh "${SSH_OPTS[@]}" "${REMOTE}" "find '${WHILLATS_DIR_REMOTE}/build' -name whillats_server -type f 2>/dev/null | head -1")
ESPEAK_DATA_PATH=$(ssh "${SSH_OPTS[@]}" "${REMOTE}" "find '${WHILLATS_DIR_REMOTE}/build' -name espeak-ng-data -type d 2>/dev/null | head -1")
echo "[deploy] whillats_server: ${WHILLATS_SERVER_PATH}"
echo "[deploy] espeak data: ${ESPEAK_DATA_PATH}"

if [ -z "${WHILLATS_SERVER_PATH}" ]; then
  echo "[deploy] ERROR: whillats_server binary not found"
  exit 1
fi

echo "[deploy] === Phase 7: Install directcall to ${DEPLOY_PATH} ==="
run_remote "mkdir -p '${DEPLOY_PATH}/lib' && cp '${WEBRTC_DIR_REMOTE}/src/out/release/directcall' '${DEPLOY_PATH}/directcall' && cp '${WEBRTC_DIR_REMOTE}/src/out/release/libdirect.so' '${DEPLOY_PATH}/lib/libdirect.so' && chmod +x '${DEPLOY_PATH}/directcall'"

run_remote "cat > '${DEPLOY_PATH}/run-directcall.sh' <<'RUNEOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR=\"\$(cd \"\$(dirname \"\${BASH_SOURCE[0]}\")\" && pwd)\"
export LD_LIBRARY_PATH=\"\${SELF_DIR}/lib:\${LD_LIBRARY_PATH:-}\"
exec stdbuf -oL \"\${SELF_DIR}/directcall\" \"\$@\"
RUNEOF
chmod +x '${DEPLOY_PATH}/run-directcall.sh'"

echo "[deploy] === Phase 8: Config, certs, models ==="
if [ -n "${CONFIG_PATH}" ] && [ -f "${CONFIG_PATH}" ]; then
  scp "${SSH_OPTS[@]}" "${CONFIG_PATH}" "${REMOTE}:/tmp/directcall-config.json"
  run_remote "mv /tmp/directcall-config.json '${DEPLOY_PATH}/config.talking-face.json'"
else
  run_remote "cat > '${DEPLOY_PATH}/config.talking-face.json' <<'CFGEOF'
{
  \"mode\": \"callee\",
  \"address\": \"0.0.0.0:3458\",
  \"user_name\": \"Slim\",
  \"room_name\": \"room101\",
  \"websocket_signaling\": true,
  \"websocket_port\": 3459,
  \"encryption\": true,
  \"whisper\": true,
  \"llama\": true,
  \"video\": true,
  \"bonjour\": false,
  \"language\": \"en\",
  \"whisper_model\": \"/opt/models/ggml-base.bin\",
  \"llama_model\": \"/opt/models/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf\",
  \"webrtc_cert_path\": \"/opt/directcall/cert.pem\",
  \"webrtc_key_path\": \"/opt/directcall/key.pem\",
  \"webrtc_speech_initial_playout_wav\": \"\",
  \"turns\": [
    [\"turn:turn.wilddolphin.us:3478?transport=udp\", \"webrtcsays.ai\", \"wilddolphin\"],
    [\"turn:turn.wilddolphin.us:5349?transport=tcp\", \"webrtcsays.ai\", \"wilddolphin\"]
  ],
  \"talking_face\": \"/opt/directcall/RobotPhoneLogo.jpeg\",
  \"whisper_threads\": 4,
  \"llama_threads\": 6,
  \"tts_threads\": 2,
  \"whispers\": { \"en\": [\"Hello, how are you?\"] }
}
CFGEOF"
fi

run_remote "if [ ! -f '${DEPLOY_PATH}/cert.pem' ] || [ ! -f '${DEPLOY_PATH}/key.pem' ]; then openssl req -x509 -newkey rsa:4096 -keyout '${DEPLOY_PATH}/key.pem' -out '${DEPLOY_PATH}/cert.pem' -sha256 -days 3650 -nodes -subj '/C=US/ST=NA/L=NA/O=Wilddolphin/OU=DirectCall/CN=directcall'; fi"

HF_AUTH=""
if [ -n "${HF_TOKEN}" ]; then
  HF_AUTH="--header 'Authorization: Bearer ${HF_TOKEN}'"
fi
run_remote "mkdir -p '${MODELS_PATH}' '${MODELS_PATH}/piper'; if [ ! -f '${MODELS_PATH}/${WHISPER_MODEL_FILE}' ]; then echo 'Downloading Whisper...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/${WHISPER_MODEL_FILE}' '${WHISPER_MODEL_URL}'; else echo 'Whisper present'; fi; if [ ! -f '${MODELS_PATH}/${LLAMA_MODEL_FILE}' ]; then echo 'Downloading LLM...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/${LLAMA_MODEL_FILE}' '${LLAMA_MODEL_URL}'; else echo 'LLM present'; fi; if [ ! -f '${MODELS_PATH}/piper/${PIPER_MODEL_FILE}' ]; then echo 'Downloading Piper...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/piper/${PIPER_MODEL_FILE}' '${PIPER_MODEL_URL}'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/piper/${PIPER_MODEL_FILE}.json' '${PIPER_MODEL_JSON_URL}'; else echo 'Piper present'; fi"

echo "[deploy] === Phase 9: TURN server ==="
if [ "${SKIP_COTURN}" = "true" ]; then
  echo "[deploy] Skipping coturn"
else
  run_remote "if ! command -v turnserver >/dev/null 2>&1; then apt-get install -y -qq coturn 2>/dev/null; fi; SERVER_IP=\$(ip -4 addr show eth0 2>/dev/null | grep -oP '(?<=inet )[\d.]+' | head -1); if [ ! -f /etc/turnserver.conf ] || ! grep -q 'wilddolphin.us' /etc/turnserver.conf 2>/dev/null; then echo 'Configuring coturn...'; cat > /etc/turnserver.conf <<TURNEOF
listening-port=3478
tls-listening-port=5349
listening-ip=\${SERVER_IP}
relay-ip=\${SERVER_IP}
min-port=49152
max-port=65535
verbose
fingerprint
lt-cred-mech
user=webrtcsays.ai:wilddolphin
realm=wilddolphin.us
server-name=wilddolphin.us
no-multicast-peers
no-rfc5780
no-stun-backward-compatibility
response-origin-only-with-rfc5780
syslog
no-cli
TURNEOF
systemctl enable coturn && systemctl restart coturn && echo 'coturn configured'; else echo 'coturn already configured'; systemctl is-active coturn || systemctl restart coturn; fi"
fi

echo "[deploy] === Phase 10: systemd service + daily restart ==="
run_remote "cat > /etc/systemd/system/${SERVICE_NAME}.service <<'SVCEOF'
[Unit]
Description=DirectCall WebRTC callee (talkingface)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=${DEPLOY_PATH}
ExecStart=${DEPLOY_PATH}/run-directcall.sh --config ${DEPLOY_PATH}/config.talking-face.json
Restart=always
RestartSec=2
Environment=LD_LIBRARY_PATH=${DEPLOY_PATH}/lib
Environment=WHILLATS_SERVER=${WHILLATS_SERVER_PATH}
Environment=PIPER_MODEL=${MODELS_PATH}/piper/${PIPER_MODEL_FILE}
Environment=ESPEAK_DATA_PATH=${ESPEAK_DATA_PATH}

[Install]
WantedBy=multi-user.target
SVCEOF

cat > /etc/systemd/system/${SERVICE_NAME}-restart.timer <<'TMREOF'
[Unit]
Description=Restart DirectCall daily at midnight

[Timer]
OnCalendar=*-*-* 00:00:00
Persistent=true

[Install]
WantedBy=timers.target
TMREOF

cat > /etc/systemd/system/${SERVICE_NAME}-restart.service <<'RSVEOF'
[Unit]
Description=Restart DirectCall service

[Service]
Type=oneshot
ExecStart=/bin/systemctl restart ${SERVICE_NAME}.service
RSVEOF

systemctl daemon-reload && systemctl enable ${SERVICE_NAME}.service && systemctl enable ${SERVICE_NAME}-restart.timer && systemctl restart ${SERVICE_NAME}.service && systemctl restart ${SERVICE_NAME}-restart.timer && echo 'systemd configured and started'"

echo "[deploy] === Phase 11: nginx WebSocket proxy ==="
run_remote "if [ -f /etc/nginx/sites-enabled/default ]; then rm -f /etc/nginx/sites-enabled/default; fi; cat > /etc/nginx/sites-enabled/directcall <<'NGXEOF'
server {
    listen 80;
    server_name _;
    return 301 https://\$host\$request_uri;
}

server {
    listen 443 ssl;
    server_name _;

    ssl_certificate /opt/directcall/cert.pem;
    ssl_certificate_key /opt/directcall/key.pem;

    location /ws {
        proxy_pass http://127.0.0.1:3459;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_read_timeout 86400;
        proxy_send_timeout 86400;
    }

    location / {
        return 404;
    }
}
NGXEOF
nginx -t && systemctl reload nginx && echo 'nginx configured'"

if [ "${ENABLE_SIGNAL_BRIDGE}" = "true" ]; then
  echo "[deploy] === Phase 12: Signal bridge ==="
  run_remote "cat > '${DEPLOY_PATH}/bridge_signal_tcp.py' <<'PYEOF'
#!/usr/bin/env python3
import json, os, socket, struct, sys, time, traceback, urllib.parse, urllib.request
from typing import Optional

SIGNAL_BASE = '${SIGNAL_BASE_URL}'
SIGNAL_ROOM = '${BRIDGE_ROOM}'
CALLEE_ROOM = '${CALLEE_ROOM}'
CALLEE_HOST = '${CALLEE_HOST}'
CALLEE_PORT = ${CALLEE_PORT}
POLL_INTERVAL = 0.2
SOCKET_TIMEOUT = 0.2

sock: Optional[socket.socket] = None
recv_buf = b''
init_sent = False

def log(msg): print(f'[bridge] {msg}', flush=True)
def signal_get(params):
    q = urllib.parse.urlencode(params)
    with urllib.request.urlopen(f'{SIGNAL_BASE}?{q}', timeout=5) as r:
        return json.loads(r.read().decode('utf-8', 'replace'))
def signal_post(body):
    data = json.dumps(body).encode('utf-8')
    req = urllib.request.Request(f'{SIGNAL_BASE}?action=post&role=callee&room={urllib.parse.quote(SIGNAL_ROOM)}', data=data, headers={'Content-Type': 'application/json'}, method='POST')
    with urllib.request.urlopen(req, timeout=5): pass
def ensure_socket():
    global sock
    if sock is not None: return
    log(f'connect tcp {CALLEE_HOST}:{CALLEE_PORT}')
    s = socket.create_connection((CALLEE_HOST, CALLEE_PORT), timeout=2)
    s.settimeout(SOCKET_TIMEOUT); sock = s
def close_socket():
    global sock, recv_buf, init_sent
    if sock: sock.close()
    sock = None; recv_buf = b''; init_sent = False
def send_framed(line):
    ensure_socket()
    payload = line.encode('utf-8')
    sock.sendall(struct.pack('!H', len(payload)) + payload)
def recv_framed_messages():
    global recv_buf
    if sock is None: return
    while True:
        try:
            chunk = sock.recv(65536)
            if not chunk: log('tcp closed'); close_socket(); return
            recv_buf += chunk
        except socket.timeout: break
        except Exception as e: log(f'recv error: {e!r}'); close_socket(); return
    while len(recv_buf) >= 2:
        pkt_len = struct.unpack('!H', recv_buf[:2])[0]
        if len(recv_buf) < 2 + pkt_len: break
        msg_bytes = recv_buf[2:2 + pkt_len]
        recv_buf = recv_buf[2 + pkt_len:]
        yield msg_bytes.decode('utf-8', 'replace')
def send_init_sequence(force=False):
    global init_sent
    ensure_socket()
    if init_sent and not force: return
    time.sleep(0.15); send_framed('HELLO'); time.sleep(0.05)
    send_framed('INVITE:{\"agent\":\"audio\",\"room_name\":\"%s\"}' % CALLEE_ROOM); init_sent = True
def process_incoming():
    if sock is None: return
    for msg in recv_framed_messages():
        try:
            if msg.startswith('ANSWER:'): signal_post({'type': 'answer', 'sdp': msg[7:]})
            elif msg.startswith('ICE:'): idx, cand = msg[4:].split(':', 1); signal_post({'type': 'ice', 'candidate': {'candidate': cand, 'sdpMLineIndex': int(idx)}})
            elif msg.startswith('LLAMA:'): signal_post({'type': 'llama', 'text': msg})
            elif msg.startswith('BYE'): signal_post({'type': 'status', 'status': 'BYE'})
            else: signal_post({'type': 'status', 'status': msg})
        except Exception as e: log(f'post err: {e!r}')
def handle_browser(msgs):
    order = {'invite': 0, 'call': 0, 'start': 0, 'offer': 1, 'face': 2, 'ice': 3, 'hangup': 9}
    for m in sorted(msgs, key=lambda m: order.get(m.get('type', ''), 5)):
        t = m.get('type')
        try:
            if t in ('invite', 'call', 'start'): send_init_sequence(force=True); signal_post({'type': 'status', 'status': 'WAITING'})
            elif t == 'offer':
                sdp = m.get('sdp', '')
                if sdp: close_socket(); send_init_sequence(force=True); signal_post({'type': 'status', 'status': 'WAITING'}); send_framed('OFFER:' + sdp)
            elif t == 'ice':
                send_init_sequence(); c = m.get('candidate') or {}; cand = c.get('candidate') if isinstance(c, dict) else None
                if cand: send_framed(f'ICE:{int(c.get(\"sdpMLineIndex\", 0))}:{cand}')
            elif t == 'hangup':
                try: send_framed('BYE')
                finally: close_socket()
        except Exception as e: log(f'handle err: {e!r}')
def main():
    log('bridge started')
    try: signal_get({'action': 'reset', 'role': 'callee', 'room': SIGNAL_ROOM})
    except Exception as e: log(f'reset failed: {e!r}')
    while True:
        try:
            polled = signal_get({'action': 'poll', 'role': 'callee', 'room': SIGNAL_ROOM})
            msgs = polled.get('messages', []) if isinstance(polled, dict) else []
            if msgs: handle_browser(msgs)
            process_incoming()
        except Exception as e: log(f'loop error: {e!r}'); close_socket()
        time.sleep(POLL_INTERVAL)
if __name__ == '__main__':
    import fcntl, atexit
    lock_fd = open('/tmp/bridge3.lock', 'w')
    try: fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB); lock_fd.write(str(os.getpid())); lock_fd.flush()
    except IOError: print('[bridge] Another instance running.', flush=True); sys.exit(1)
    atexit.register(lambda: os.unlink('/tmp/bridge3.lock'))
    main()
PYEOF
chmod +x '${DEPLOY_PATH}/bridge_signal_tcp.py'

cat > /etc/systemd/system/${BRIDGE_SERVICE_NAME}.service <<'EOF'
[Unit]
Description=Bridge signal.php to directcall TCP callee
After=network-online.target ${SERVICE_NAME}.service
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=${DEPLOY_PATH}
ExecStart=/usr/bin/python3 ${DEPLOY_PATH}/bridge_signal_tcp.py
Restart=always
RestartSec=1

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload && systemctl enable ${BRIDGE_SERVICE_NAME} && systemctl restart ${BRIDGE_SERVICE_NAME} && echo 'bridge configured'"
fi

echo "[deploy] === Phase 13: Stop legacy services ==="
run_remote "systemctl stop directcall.service 2>/dev/null; systemctl disable directcall.service 2>/dev/null; systemctl stop directcall-bridge.service 2>/dev/null; systemctl disable directcall-bridge.service 2>/dev/null; echo 'old services stopped'"

echo "[deploy] === Done ==="
run_remote "echo '--- Service status ---' && systemctl --no-pager status ${SERVICE_NAME}.service | head -15 && echo '--- Timer ---' && systemctl --no-pager status ${SERVICE_NAME}-restart.timer | head -8 && echo '--- Ports ---' && ss -tlnp | grep -E '3458|3459' && echo '--- Process ---' && ps aux | grep directcall | grep -v grep"
