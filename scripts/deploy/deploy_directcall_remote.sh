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
ENABLE_SIGNAL_BRIDGE="${ENABLE_SIGNAL_BRIDGE:-true}"
BRIDGE_SERVICE_NAME="${BRIDGE_SERVICE_NAME:-directcall-bridge}"
SIGNAL_BASE_URL="${SIGNAL_BASE_URL:-https://www.wilddolphin.us/signal.php}"
BRIDGE_ROOM="${BRIDGE_ROOM:-testroom}"
CALLEE_HOST="${CALLEE_HOST:-127.0.0.1}"
CALLEE_PORT="${CALLEE_PORT:-3456}"
CALLEE_ROOM="${CALLEE_ROOM:-room101}"
MODELS_PATH="${MODELS_PATH:-/opt/models}"
HF_TOKEN="${HF_TOKEN:-}"
WHISPER_MODEL_URL="${WHISPER_MODEL_URL:-https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin}"
LLAMA_MODEL_URL="${LLAMA_MODEL_URL:-https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/main/Qwen3.5-9B-Q3_K_M.gguf}"
WHISPER_MODEL_FILE="${WHISPER_MODEL_FILE:-ggml-small.bin}"
LLAMA_MODEL_FILE="${LLAMA_MODEL_FILE:-Qwen3.5-9B-Q3_K_M.gguf}"

if [ ! -f "${ARTIFACT_PATH}" ]; then
  echo "Artifact not found: ${ARTIFACT_PATH}"
  exit 1
fi

if [ ! -f "${SSH_KEY_PATH}" ]; then
  echo "SSH key not found: ${SSH_KEY_PATH}"
  exit 1
fi

if [ ! -f "${CONFIG_PATH}" ]; then
  echo "Config not found: ${CONFIG_PATH}"
  exit 1
fi

SSH_OPTS=(-i "${SSH_KEY_PATH}" -p "${DEPLOY_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SCP_OPTS=(-i "${SSH_KEY_PATH}" -P "${DEPLOY_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
REMOTE="${DEPLOY_USER}@${DEPLOY_HOST}"
TMP_TAR="/tmp/directcall-linux.tar.gz"
TMP_CONFIG="/tmp/directcall-config.json"

echo "[deploy] Uploading artifact to ${REMOTE}:${TMP_TAR}"
scp "${SCP_OPTS[@]}" "${ARTIFACT_PATH}" "${REMOTE}:${TMP_TAR}"
echo "[deploy] Uploading config to ${REMOTE}:${TMP_CONFIG}"
scp "${SCP_OPTS[@]}" "${CONFIG_PATH}" "${REMOTE}:${TMP_CONFIG}"

echo "[deploy] Installing on remote host"
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
ssh "${SSH_OPTS[@]}" "${REMOTE}" "mkdir -p '${MODELS_PATH}'; if [ ! -f '${MODELS_PATH}/${WHISPER_MODEL_FILE}' ]; then echo 'Downloading Whisper model...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/${WHISPER_MODEL_FILE}' '${WHISPER_MODEL_URL}'; else echo 'Whisper model already present'; fi; if [ ! -f '${MODELS_PATH}/${LLAMA_MODEL_FILE}' ]; then echo 'Downloading LLM model...'; curl -L ${HF_AUTH} -o '${MODELS_PATH}/${LLAMA_MODEL_FILE}' '${LLAMA_MODEL_URL}'; else echo 'LLM model already present'; fi; ls -lh '${MODELS_PATH}/'"

STYLETTS2_HF_REPO="DDATT/StyleTTS2-ONNX-Cpp"
STYLETTS2_MODEL_DIR="/opt/whillats/trained_models"
ONNXRUNTIME_VERSION="1.17.3"
echo "[deploy] Downloading StyleTTS2 ONNX models if not present"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "mkdir -p '${STYLETTS2_MODEL_DIR}'; if [ ! -f '${STYLETTS2_MODEL_DIR}/final_simp.onnx' ]; then echo 'Downloading StyleTTS2 models...'; HF_BASE='https://huggingface.co/${STYLETTS2_HF_REPO}/resolve/main'; for f in bert_encoder.onnx plbert_simp.onnx final_simp.onnx ref_s.bin ref_p.bin; do echo \"  \$f\"; curl -L -s ${HF_AUTH} -o '${STYLETTS2_MODEL_DIR}/'\$f \"\${HF_BASE}/\$f\"; done; else echo 'StyleTTS2 models already present'; fi; ls -lh '${STYLETTS2_MODEL_DIR}/'"

ONNXRUNTIME_LIB_DIR="/opt/whillats/build/_deps/onnxruntime-src/lib"
echo "[deploy] Downloading ONNX Runtime if not present"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "mkdir -p '${ONNXRUNTIME_LIB_DIR}'; if [ ! -f '${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so' ]; then echo 'Downloading ONNX Runtime ${ONNXRUNTIME_VERSION}...'; curl -L -s -o /tmp/onnxruntime.tgz 'https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz' && tar -xzf /tmp/onnxruntime.tgz -C /tmp && cp /tmp/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}/lib/libonnxruntime.so* '${ONNXRUNTIME_LIB_DIR}/' && rm -rf /tmp/onnxruntime* && echo 'ONNX Runtime installed'; else echo 'ONNX Runtime already present'; fi"

echo "[deploy] Copying ONNX Runtime and whillats libs to deploy path"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "cp -n '${ONNXRUNTIME_LIB_DIR}'/libonnxruntime.so* '${DEPLOY_PATH}/lib/' 2>/dev/null || true; if [ -f /opt/whillats/build/lib/Release/libwhillats.so ]; then cp /opt/whillats/build/lib/Release/libwhillats.so '${DEPLOY_PATH}/lib/libwhillats.so'; echo 'Copied server-built libwhillats.so'; fi"

echo "[deploy] Copying talking face image"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "if [ ! -f '${DEPLOY_PATH}/RobotPhoneLogo.jpeg' ]; then if [ -f /opt/directcall/RobotPhoneLogo.jpeg ]; then echo 'Face image already present'; else echo 'Face image missing - will use placeholder'; fi; fi"

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

WHILLATS_BRANCH="${WHILLATS_BRANCH:-talkingface}"
WHILLATS_DIR="/opt/whillats"
echo "[deploy] Building whillats on remote (branch: ${WHILLATS_BRANCH})"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "apt-get install -y -qq build-essential cmake git autoconf automake libtool pkg-config libasound2-dev libgomp1 espeak-ng espeak-ng-data 2>/dev/null; mkdir -p /usr/local/share && ESPEAK_SRC=\$(find /usr/lib -name espeak-ng-data -type d 2>/dev/null | head -1); [ -n \"\${ESPEAK_SRC}\" ] && ln -sf \"\${ESPEAK_SRC}\" /usr/local/share/espeak-ng-data 2>/dev/null || true; if [ ! -d '${WHILLATS_DIR}/.git' ]; then rm -rf '${WHILLATS_DIR}'; git clone --branch '${WHILLATS_BRANCH}' --recursive https://github.com/wilddolphin2025/whillats.git '${WHILLATS_DIR}'; else cd '${WHILLATS_DIR}' && git remote set-url origin https://github.com/wilddolphin2025/whillats.git && git fetch origin '${WHILLATS_BRANCH}' && git checkout -B '${WHILLATS_BRANCH}' 'origin/${WHILLATS_BRANCH}' && git submodule update --init --recursive; fi && cd '${WHILLATS_DIR}' && cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF -DWHISPER_CUDA=OFF -DGGML_CUBLAS=OFF -DLLAMA_CUDA=OFF 2>&1 | tail -3 && cmake --build build --config Release --parallel \$(nproc) --target whillats 2>&1 | tail -5 && cp build/lib/Release/libwhillats.so '${DEPLOY_PATH}/lib/libwhillats.so' && cp -n build/_deps/onnxruntime-src/lib/libonnxruntime.so* '${DEPLOY_PATH}/lib/' 2>/dev/null || true && echo 'whillats built and installed'"

echo "[deploy] Writing systemd unit"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > /etc/systemd/system/${SERVICE_NAME}.service <<'EOF'
[Unit]
Description=DirectCall WebRTC callee
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=${DEPLOY_PATH}
ExecStart=${DEPLOY_PATH}/run-directcall.sh --config ${DEPLOY_PATH}/config.talking-face.json
Restart=always
RestartSec=2
Environment=LD_LIBRARY_PATH=${DEPLOY_PATH}/lib
Environment=STYLETTS2_MODEL_DIR=/opt/whillats/trained_models
Environment=ESPEAK_DATA_PATH=/opt/whillats/build/bin/Release/espeak-ng-data

[Install]
WantedBy=multi-user.target
EOF"

echo "[deploy] Patching run-directcall.sh for unbuffered stdout"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > '${DEPLOY_PATH}/run-directcall.sh' <<'RUNEOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR=\"\$(cd \"\$(dirname \"\${BASH_SOURCE[0]}\")\" && pwd)\"
export LD_LIBRARY_PATH=\"\${SELF_DIR}/lib:\${LD_LIBRARY_PATH:-}\"
exec stdbuf -oL \"\${SELF_DIR}/directcall\" \"\$@\"
RUNEOF
chmod +x '${DEPLOY_PATH}/run-directcall.sh'"

echo "[deploy] Reloading and restarting ${SERVICE_NAME}"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "systemctl daemon-reload && systemctl enable ${SERVICE_NAME} && systemctl restart ${SERVICE_NAME} && systemctl --no-pager --full status ${SERVICE_NAME} | sed -n '1,30p'"

if [ "${ENABLE_SIGNAL_BRIDGE}" = "true" ]; then
  echo "[deploy] Installing signal bridge script (v2 with framing, reordering, session management)"
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > '${DEPLOY_PATH}/bridge_signal_tcp.py' <<'PY'
#!/usr/bin/env python3
\"\"\"Bridge between signal.php (HTTPS polling) and directcall TCP callee.

directcall uses WebRTC AsyncTCPSocket framing: every TCP message is
preceded by a 2-byte big-endian length prefix (uint16).  This bridge
encodes outgoing messages and decodes incoming ones accordingly.

Protocol order: HELLO -> INVITE -> OFFER -> ICE -> ...
\"\"\"
import json
import socket
import struct
import time
import traceback
import urllib.parse
import urllib.request
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


def log(msg: str):
    print(f'[bridge] {msg}', flush=True)


def signal_get(params: dict):
    q = urllib.parse.urlencode(params)
    with urllib.request.urlopen(f'{SIGNAL_BASE}?{q}', timeout=5) as r:
        return json.loads(r.read().decode('utf-8', 'replace'))


def signal_post(body: dict):
    data = json.dumps(body).encode('utf-8')
    msg_type = body.get('type', '?')
    log(f'signal >> post {msg_type} ({len(data)} bytes)')
    req = urllib.request.Request(
        f'{SIGNAL_BASE}?action=post&role=callee&room={urllib.parse.quote(SIGNAL_ROOM)}',
        data=data,
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        resp = r.read()
        log(f'signal >> post {msg_type} OK')


def ensure_socket():
    global sock
    if sock is not None:
        return
    log(f'connect tcp {CALLEE_HOST}:{CALLEE_PORT}')
    s = socket.create_connection((CALLEE_HOST, CALLEE_PORT), timeout=2)
    s.settimeout(SOCKET_TIMEOUT)
    sock = s


def close_socket():
    global sock, recv_buf, init_sent
    if sock is not None:
        try:
            sock.close()
        except Exception:
            pass
    sock = None
    recv_buf = b''
    init_sent = False


def send_framed(line: str):
    \"\"\"Send a message with 2-byte big-endian length prefix (AsyncTCPSocket wire format).\"\"\"
    ensure_socket()
    assert sock is not None
    payload = line.encode('utf-8')
    header = struct.pack('!H', len(payload))
    sock.sendall(header + payload)
    log(f'tcp >> [{len(payload)}] {line[:120]}')


def recv_framed_messages():
    \"\"\"Read from TCP and yield complete length-prefixed messages.\"\"\"
    global recv_buf
    if sock is None:
        return
    while True:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                log('tcp closed by callee')
                close_socket()
                return
            recv_buf += chunk
        except socket.timeout:
            break
        except Exception as e:
            log(f'tcp recv error: {e!r}')
            close_socket()
            return

    while len(recv_buf) >= 2:
        pkt_len = struct.unpack('!H', recv_buf[:2])[0]
        if len(recv_buf) < 2 + pkt_len:
            break
        msg_bytes = recv_buf[2:2 + pkt_len]
        recv_buf = recv_buf[2 + pkt_len:]
        yield msg_bytes.decode('utf-8', 'replace')


def send_init_sequence(force=False):
    global init_sent
    ensure_socket()
    if init_sent and not force:
        return
    time.sleep(0.15)
    send_framed('HELLO')
    time.sleep(0.05)
    send_framed('INVITE:{\"agent\":\"audio\",\"room_name\":\"%s\"}' % CALLEE_ROOM)
    init_sent = True


def process_incoming_from_callee():
    if sock is None:
        return
    for msg in recv_framed_messages():
        log(f'tcp << {msg[:200]}')
        try:
            if msg.startswith('ANSWER:'):
                signal_post({'type': 'answer', 'sdp': msg[len('ANSWER:'):]})
            elif msg.startswith('ICE:'):
                payload = msg[len('ICE:'):]
                idx, cand = payload.split(':', 1)
                signal_post({'type': 'ice', 'candidate': {'candidate': cand, 'sdpMLineIndex': int(idx), 'sdpMid': '0'}})
            elif msg.startswith('LLAMA:'):
                signal_post({'type': 'llama', 'text': msg})
            elif msg.startswith('200 OK'):
                signal_post({'type': 'status', 'status': '200 OK'})
            elif msg.startswith('486 Busy Here'):
                signal_post({'type': 'status', 'status': '486 Busy Here'})
            elif msg.startswith('WAITING'):
                signal_post({'type': 'status', 'status': 'WAITING'})
            elif msg.startswith('BYE'):
                signal_post({'type': 'status', 'status': 'BYE'})
            else:
                signal_post({'type': 'status', 'status': msg})
        except Exception as e:
            log(f'signal_post from callee failed: {e!r}')


def _msg_sort_key(m: dict) -> int:
    \"\"\"Sort key enforcing protocol order: offer before ice, hangup last.\"\"\"
    t = m.get('type', '')
    order = {'invite': 0, 'call': 0, 'start': 0, 'offer': 1, 'face': 2, 'ice': 3, 'hangup': 9}
    return order.get(t, 5)


def handle_browser_messages(msgs: List[dict]):
    sorted_msgs = sorted(msgs, key=_msg_sort_key)
    if [m.get('type') for m in msgs] != [m.get('type') for m in sorted_msgs]:
        log(f'reordered: {[m.get(\"type\") for m in msgs]} -> {[m.get(\"type\") for m in sorted_msgs]}')

    for m in sorted_msgs:
        t = m.get('type')
        log(f'signal << {t}')
        try:
            if t in ('invite', 'call', 'start'):
                send_init_sequence(force=True)
                signal_post({'type': 'status', 'status': 'WAITING'})
            elif t == 'offer':
                sdp = m.get('sdp', '')
                if sdp:
                    close_socket()
                    send_init_sequence(force=True)
                    signal_post({'type': 'status', 'status': 'WAITING'})
                    send_framed('OFFER:' + sdp)
            elif t == 'ice':
                send_init_sequence()
                c = m.get('candidate') or {}
                cand = c.get('candidate') if isinstance(c, dict) else None
                idx = c.get('sdpMLineIndex', 0) if isinstance(c, dict) else 0
                if cand:
                    send_framed(f'ICE:{int(idx)}:{cand}')
            elif t == 'face':
                data = m.get('data', '')
                if data:
                    send_framed('FACE:' + data)
            elif t == 'hangup':
                try:
                    send_framed('BYE')
                finally:
                    close_socket()
        except Exception as e:
            log(f'handle msg error: {e!r}')
            log(traceback.format_exc())


def main():
    log('bridge started')

    while True:
        try:
            polled = signal_get({'action': 'poll', 'role': 'callee', 'room': SIGNAL_ROOM})
            msgs = polled.get('messages', []) if isinstance(polled, dict) else []
            if msgs:
                handle_browser_messages(msgs)
            process_incoming_from_callee()
        except Exception as e:
            log(f'main loop error: {e!r}')
            log(traceback.format_exc())
            close_socket()
        time.sleep(POLL_INTERVAL)


if __name__ == '__main__':
    main()
PY
chmod +x '${DEPLOY_PATH}/bridge_signal_tcp.py'"

  echo "[deploy] Writing bridge systemd unit"
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > /etc/systemd/system/${BRIDGE_SERVICE_NAME}.service <<'EOF'
[Unit]
Description=Bridge signal.php room traffic to directcall TCP callee
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

echo "[deploy] Done"
