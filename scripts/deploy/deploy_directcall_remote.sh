#!/usr/bin/env bash
set -euo pipefail

ARTIFACT_PATH="${1:-dist/directcall-linux.tar.gz}"
CONFIG_PATH="${2:-dist/directcall-config.json}"
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
WHILLATS_BRANCH="${WHILLATS_BRANCH:-talkingface}"
WHILLATS_DIR_REMOTE="/opt/whillats3"
WHISPER_MODEL_URL="${WHISPER_MODEL_URL:-https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin}"
WHISPER_MODEL_FILE="${WHISPER_MODEL_FILE:-ggml-base.bin}"
LLAMA_MODEL_URL="${LLAMA_MODEL_URL:-https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf}"
LLAMA_MODEL_FILE="${LLAMA_MODEL_FILE:-Qwen2.5-1.5B-Instruct-Q4_K_M.gguf}"
PIPER_MODEL_URL="${PIPER_MODEL_URL:-https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/low/en_US-lessac-low.onnx}"
PIPER_MODEL_JSON_URL="${PIPER_MODEL_JSON_URL:-https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/low/en_US-lessac-low.onnx.json}"
PIPER_MODEL_FILE="${PIPER_MODEL_FILE:-en_US-lessac-low.onnx}"
DEPLOY_DRY_RUN="${DEPLOY_DRY_RUN:-false}"

deploy_dry_run() {
  case "${DEPLOY_DRY_RUN}" in
    true|1|yes|TRUE|YES) return 0 ;;
    *) return 1 ;;
  esac
}

if [ ! -f "${ARTIFACT_PATH}" ]; then
  echo "Artifact not found: ${ARTIFACT_PATH}"
  exit 1
fi

if [ ! -f "${CONFIG_PATH}" ]; then
  echo "Config not found: ${CONFIG_PATH}"
  exit 1
fi

if deploy_dry_run; then
  echo "[deploy] DRY RUN: build artifact validated; skipping SSH — remote host and binaries are unchanged."
  echo "[deploy] Artifact: ${ARTIFACT_PATH} ($(wc -c < "${ARTIFACT_PATH}") bytes)"
  echo "[deploy] Would deploy to: ${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH} (service ${SERVICE_NAME})"
  ls -la "${ARTIFACT_PATH}" "${CONFIG_PATH}"
  exit 0
fi

if [ ! -f "${SSH_KEY_PATH}" ]; then
  echo "SSH key not found: ${SSH_KEY_PATH}"
  exit 1
fi

SSH_OPTS=(-i "${SSH_KEY_PATH}" -p "${DEPLOY_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SCP_OPTS=(-i "${SSH_KEY_PATH}" -P "${DEPLOY_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
REMOTE="${DEPLOY_USER}@${DEPLOY_HOST}"
TMP_TAR="/tmp/directcall-linux.tar.gz"
TMP_CONFIG="/tmp/directcall-config.json"

run_remote() {
  if [ "${DEPLOY_USER}" = "root" ]; then
    ssh "${SSH_OPTS[@]}" "${REMOTE}" "$1"
  else
    ssh "${SSH_OPTS[@]}" "${REMOTE}" "sudo bash -c '$1'"
  fi
}

echo "[deploy] Preparing remote host permissions"
if [ "${DEPLOY_USER}" != "root" ]; then
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "sudo mkdir -p '${DEPLOY_PATH}' '${WHILLATS_DIR_REMOTE}' '${MODELS_PATH}' && sudo chown -R '${DEPLOY_USER}:${DEPLOY_USER}' '${DEPLOY_PATH}' '${WHILLATS_DIR_REMOTE}' '${MODELS_PATH}'"
fi

echo "[deploy] Uploading artifact to ${REMOTE}:${TMP_TAR}"
scp "${SCP_OPTS[@]}" "${ARTIFACT_PATH}" "${REMOTE}:${TMP_TAR}"
echo "[deploy] Uploading config to ${REMOTE}:${TMP_CONFIG}"
scp "${SCP_OPTS[@]}" "${CONFIG_PATH}" "${REMOTE}:${TMP_CONFIG}"

echo "[deploy] Installing directcall on remote host at ${DEPLOY_PATH}"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "mkdir -p '${DEPLOY_PATH}' && tar -xzf '${TMP_TAR}' -C '${DEPLOY_PATH}' --strip-components=1 && mv '${TMP_CONFIG}' '${DEPLOY_PATH}/config.talking-face.json' && chmod +x '${DEPLOY_PATH}/directcall' '${DEPLOY_PATH}/run-directcall.sh' && rm -f '${TMP_TAR}'"

echo "[deploy] Ensuring runtime certificate files exist"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "if [ ! -f '${DEPLOY_PATH}/cert.pem' ] || [ ! -f '${DEPLOY_PATH}/key.pem' ]; then openssl req -x509 -newkey rsa:4096 -keyout '${DEPLOY_PATH}/key.pem' -out '${DEPLOY_PATH}/cert.pem' -sha256 -days 3650 -nodes -subj '/C=US/ST=NA/L=NA/O=Wilddolphin/OU=DirectCall/CN=directcall'; fi"

echo "[deploy] Ensuring espeak-ng data is available"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "apt-get install -y -qq espeak-ng espeak-ng-data 2>/dev/null || true; if [ ! -e /usr/local/share/espeak-ng-data ]; then ESPEAK_SRC=\$(find /usr/lib -name espeak-ng-data -type d 2>/dev/null | head -1); if [ -n \"\${ESPEAK_SRC}\" ]; then mkdir -p /usr/local/share && ln -sf \"\${ESPEAK_SRC}\" /usr/local/share/espeak-ng-data && echo 'espeak-ng data linked'; fi; fi"

echo "[deploy] Downloading AI models if not present"
HF_AUTH=""
if [ -n "${HF_TOKEN}" ]; then
  HF_AUTH="--header 'Authorization: Bearer ${HF_TOKEN}'"
fi
ssh "${SSH_OPTS[@]}" "${REMOTE}" "mkdir -p '${MODELS_PATH}' '${MODELS_PATH}/piper'; if [ ! -f '${MODELS_PATH}/${WHISPER_MODEL_FILE}' ]; then echo 'Downloading Whisper model (${WHISPER_MODEL_FILE})...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/${WHISPER_MODEL_FILE}' '${WHISPER_MODEL_URL}'; else echo 'Whisper model already present'; fi; if [ ! -f '${MODELS_PATH}/${LLAMA_MODEL_FILE}' ]; then echo 'Downloading LLM model (${LLAMA_MODEL_FILE})...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/${LLAMA_MODEL_FILE}' '${LLAMA_MODEL_URL}'; else echo 'LLM model already present'; fi; if [ ! -f '${MODELS_PATH}/piper/${PIPER_MODEL_FILE}' ]; then echo 'Downloading Piper TTS model...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/piper/${PIPER_MODEL_FILE}' '${PIPER_MODEL_URL}'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/piper/${PIPER_MODEL_FILE}.json' '${PIPER_MODEL_JSON_URL}'; else echo 'Piper model already present'; fi; ls -lh '${MODELS_PATH}/' '${MODELS_PATH}/piper/'"

if [ "${SKIP_COTURN}" = "true" ]; then
  echo "[deploy] Skipping coturn (TURN server already configured)"
else
echo "[deploy] Installing and configuring coturn TURN server"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "if ! command -v turnserver >/dev/null 2>&1; then apt-get install -y -qq coturn 2>/dev/null; fi; SERVER_IP=\$(ip -4 addr show eth0 | grep -oP '(?<=inet )[\d.]+' | head -1); if [ ! -f /etc/turnserver.conf ] || ! grep -q 'wilddolphin.us' /etc/turnserver.conf 2>/dev/null; then echo 'Configuring coturn...'; if [ -f '${DEPLOY_PATH}/cert.pem' ]; then cp '${DEPLOY_PATH}/cert.pem' /etc/turn_cert.pem; cp '${DEPLOY_PATH}/key.pem' /etc/turn_key.pem; chown turnserver:turnserver /etc/turn_cert.pem /etc/turn_key.pem 2>/dev/null || true; chmod 600 /etc/turn_key.pem; fi; cat > /etc/turnserver.conf <<TURNEOF
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
cert=/etc/turn_cert.pem
pkey=/etc/turn_key.pem
no-multicast-peers
no-rfc5780
no-stun-backward-compatibility
response-origin-only-with-rfc5780
syslog
no-cli
TURNEOF
systemctl enable coturn && systemctl restart coturn; echo 'coturn configured'; else echo 'coturn already configured'; systemctl is-active coturn || systemctl restart coturn; fi"
fi

echo "[deploy] Building whillats_server on remote (branch: ${WHILLATS_BRANCH})"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "apt-get install -y -qq build-essential cmake git autoconf automake libtool pkg-config libasound2-dev libgomp1 espeak-ng espeak-ng-data 2>/dev/null; mkdir -p /usr/local/share && ESPEAK_SRC=\$(find /usr/lib -name espeak-ng-data -type d 2>/dev/null | head -1); [ -n \"\${ESPEAK_SRC}\" ] && ln -sf \"\${ESPEAK_SRC}\" /usr/local/share/espeak-ng-data 2>/dev/null || true; if [ ! -d '${WHILLATS_DIR_REMOTE}/.git' ]; then rm -rf '${WHILLATS_DIR_REMOTE}'; git clone --branch '${WHILLATS_BRANCH}' --recursive https://github.com/wilddolphin2025/whillats.git '${WHILLATS_DIR_REMOTE}'; else cd '${WHILLATS_DIR_REMOTE}' && git remote set-url origin https://github.com/wilddolphin2025/whillats.git && git fetch origin '${WHILLATS_BRANCH}' && git checkout -B '${WHILLATS_BRANCH}' 'origin/${WHILLATS_BRANCH}' && git submodule update --init --recursive; fi && cd '${WHILLATS_DIR_REMOTE}' && CUDA_FLAG='OFF'; if command -v nvcc >/dev/null 2>&1; then CUDA_FLAG='ON'; echo 'CUDA detected — enabling GPU acceleration'; fi && cmake -B build -DCMAKE_BUILD_TYPE=Release -DWHILLATS_PIPER=ON -DWHILLATS_OLD_ABI=ON -DGGML_CUDA=\${CUDA_FLAG} 2>&1 | tail -5 && cmake --build build --config Release --parallel \$(nproc) --target whillats_server 2>&1 | tail -10 && echo 'whillats_server built' && find build -name whillats_server -type f 2>/dev/null"

echo "[deploy] Ensuring espeak-ng data is complete (phontab, phondata, etc.)"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "ESPEAK_SYS=\$(find /usr/lib -name espeak-ng-data -type d 2>/dev/null | head -1); ESPEAK_BUILD=\$(find '${WHILLATS_DIR_REMOTE}/build' -name espeak-ng-data -type d 2>/dev/null | head -1); if [ -n \"\${ESPEAK_SYS}\" ] && [ -n \"\${ESPEAK_BUILD}\" ]; then cp -a \"\${ESPEAK_SYS}/\"* \"\${ESPEAK_BUILD}/\" && echo 'espeak-ng data synced from system'; elif [ -n \"\${ESPEAK_SYS}\" ]; then mkdir -p '${WHILLATS_DIR_REMOTE}/build/bin/Release/espeak-ng-data' && cp -a \"\${ESPEAK_SYS}/\"* '${WHILLATS_DIR_REMOTE}/build/bin/Release/espeak-ng-data/' && echo 'espeak-ng data copied from system'; fi && ls '${WHILLATS_DIR_REMOTE}/build/bin/Release/espeak-ng-data/phontab' && echo 'phontab OK'"

echo "[deploy] Locating whillats_server and espeak data"
WHILLATS_SERVER_PATH=$(ssh "${SSH_OPTS[@]}" "${REMOTE}" "find '${WHILLATS_DIR_REMOTE}/build' -name whillats_server -type f 2>/dev/null | head -1")
ESPEAK_DATA_PATH=$(ssh "${SSH_OPTS[@]}" "${REMOTE}" "find '${WHILLATS_DIR_REMOTE}/build' -name espeak-ng-data -type d 2>/dev/null | head -1")
echo "[deploy] whillats_server: ${WHILLATS_SERVER_PATH}"
echo "[deploy] espeak data: ${ESPEAK_DATA_PATH}"

if [ -z "${WHILLATS_SERVER_PATH}" ]; then
  echo "[deploy] ERROR: whillats_server binary not found after build"
  exit 1
fi

echo "[deploy] Writing systemd unit for ${SERVICE_NAME}"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > /etc/systemd/system/${SERVICE_NAME}.service <<EOF
[Unit]
Description=DirectCall3 WebRTC callee (talkingface)
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
EOF"

echo "[deploy] Reloading and restarting ${SERVICE_NAME}"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "systemctl daemon-reload && systemctl enable ${SERVICE_NAME} && systemctl restart ${SERVICE_NAME} && sleep 2 && systemctl --no-pager --full status ${SERVICE_NAME} | sed -n '1,30p'"

if [ "${ENABLE_SIGNAL_BRIDGE}" = "true" ]; then
  echo "[deploy] Installing signal bridge script"
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > '${DEPLOY_PATH}/bridge_signal_tcp.py' <<'PY'
#!/usr/bin/env python3
\"\"\"Bridge between signal.php (HTTPS polling) and directcall TCP callee.\"\"\"
import json, os, socket, struct, sys, time, traceback, urllib.parse, urllib.request
from typing import Optional, List

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
    req = urllib.request.Request(
        f'{SIGNAL_BASE}?action=post&role=callee&room={urllib.parse.quote(SIGNAL_ROOM)}',
        data=data, headers={'Content-Type': 'application/json'}, method='POST')
    with urllib.request.urlopen(req, timeout=5): pass

def ensure_socket():
    global sock
    if sock is not None: return
    log(f'connect tcp {CALLEE_HOST}:{CALLEE_PORT}')
    s = socket.create_connection((CALLEE_HOST, CALLEE_PORT), timeout=2)
    s.settimeout(SOCKET_TIMEOUT)
    sock = s

def close_socket():
    global sock, recv_buf, init_sent
    if sock: sock.close()
    sock = None; recv_buf = b''; init_sent = False

def send_framed(line):
    ensure_socket()
    payload = line.encode('utf-8')
    sock.sendall(struct.pack('!H', len(payload)) + payload)
    log(f'tcp >> [{len(payload)}] {line[:120]}')

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
    time.sleep(0.15)
    send_framed('HELLO')
    time.sleep(0.05)
    send_framed('INVITE:{\"agent\":\"audio\",\"room_name\":\"%s\"}' % CALLEE_ROOM)
    init_sent = True

def process_incoming_from_callee():
    if sock is None: return
    for msg in recv_framed_messages():
        log(f'tcp << {msg[:200]}')
        try:
            if msg.startswith('ANSWER:'): signal_post({'type': 'answer', 'sdp': msg[7:]})
            elif msg.startswith('ICE:'): idx, cand = msg[4:].split(':', 1); signal_post({'type': 'ice', 'candidate': {'candidate': cand, 'sdpMLineIndex': int(idx), 'sdpMid': '0'}})
            elif msg.startswith('LLAMA:'): signal_post({'type': 'llama', 'text': msg})
            elif msg.startswith('BYE'): signal_post({'type': 'status', 'status': 'BYE'})
            else: signal_post({'type': 'status', 'status': msg})
        except Exception as e: log(f'signal_post err: {e!r}')

def handle_browser_messages(msgs):
    order = {'invite': 0, 'call': 0, 'start': 0, 'offer': 1, 'face': 2, 'ice': 3, 'hangup': 9}
    for m in sorted(msgs, key=lambda m: order.get(m.get('type', ''), 5)):
        t = m.get('type'); log(f'signal << {t}')
        try:
            if t in ('invite', 'call', 'start'): send_init_sequence(force=True); signal_post({'type': 'status', 'status': 'WAITING'})
            elif t == 'offer':
                sdp = m.get('sdp', '')
                if sdp: close_socket(); send_init_sequence(force=True); signal_post({'type': 'status', 'status': 'WAITING'}); send_framed('OFFER:' + sdp)
            elif t == 'ice':
                send_init_sequence(); c = m.get('candidate') or {}
                cand = c.get('candidate') if isinstance(c, dict) else None
                if cand: send_framed(f'ICE:{int(c.get("sdpMLineIndex", 0))}:{cand}')
            elif t == 'hangup':
                try: send_framed('BYE')
                finally: close_socket()
        except Exception as e: log(f'handle err: {e!r}'); log(traceback.format_exc())

def main():
    log('bridge started')
    try: signal_get({'action': 'reset', 'role': 'callee', 'room': SIGNAL_ROOM}); log(f'reset room {SIGNAL_ROOM}')
    except Exception as e: log(f'reset failed: {e!r}')
    while True:
        try:
            polled = signal_get({'action': 'poll', 'role': 'callee', 'room': SIGNAL_ROOM})
            msgs = polled.get('messages', []) if isinstance(polled, dict) else []
            if msgs: handle_browser_messages(msgs)
            process_incoming_from_callee()
        except Exception as e: log(f'loop error: {e!r}'); close_socket()
        time.sleep(POLL_INTERVAL)

if __name__ == '__main__':
    import fcntl, atexit
    lock_path = '/tmp/bridge3.lock'
    lock_fd = open(lock_path, 'w')
    try: fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB); lock_fd.write(str(os.getpid())); lock_fd.flush()
    except IOError: print('[bridge] Another instance running.', flush=True); sys.exit(1)
    atexit.register(lambda: os.unlink(lock_path))
    main()
PY
chmod +x '${DEPLOY_PATH}/bridge_signal_tcp.py'"

  echo "[deploy] Writing bridge systemd unit"
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > /etc/systemd/system/${BRIDGE_SERVICE_NAME}.service <<'EOF'
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
EOF"

  echo "[deploy] Reloading and restarting ${BRIDGE_SERVICE_NAME}"
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "systemctl daemon-reload && systemctl enable ${BRIDGE_SERVICE_NAME} && systemctl restart ${BRIDGE_SERVICE_NAME} && systemctl --no-pager --full status ${BRIDGE_SERVICE_NAME} | sed -n '1,30p'"
fi

echo "[deploy] Updating nginx to proxy /ws to directcall websocket port (3459)"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "if [ -f /etc/nginx/sites-enabled/directcall ]; then sed -i 's|proxy_pass http://127.0.0.1:3457;|proxy_pass http://127.0.0.1:3459;|g' /etc/nginx/sites-enabled/directcall && nginx -t && systemctl reload nginx && echo 'nginx updated: /ws -> 3459'; else echo 'nginx config not found at /etc/nginx/sites-enabled/directcall'; fi"

echo "[deploy] Stopping old directcall service (kept installed, not destroyed)"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "systemctl stop directcall.service 2>/dev/null; systemctl disable directcall.service 2>/dev/null; systemctl stop directcall-bridge.service 2>/dev/null; systemctl disable directcall-bridge.service 2>/dev/null; echo 'old services stopped and disabled (files in /opt/directcall preserved)'"

echo "[deploy] Done — ${SERVICE_NAME} deployed to ${DEPLOY_PATH}"
echo "[deploy] Old directcall at /opt/directcall preserved (service stopped)"
