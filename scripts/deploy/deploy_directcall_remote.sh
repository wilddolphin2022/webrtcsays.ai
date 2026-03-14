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
ENABLE_SIGNAL_BRIDGE="${ENABLE_SIGNAL_BRIDGE:-true}"
BRIDGE_SERVICE_NAME="${BRIDGE_SERVICE_NAME:-directcall-bridge}"
SIGNAL_BASE_URL="${SIGNAL_BASE_URL:-https://www.wilddolphin.us/signal.php}"
BRIDGE_ROOM="${BRIDGE_ROOM:-testroom}"
CALLEE_HOST="${CALLEE_HOST:-127.0.0.1}"
CALLEE_PORT="${CALLEE_PORT:-3456}"

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
REMOTE="${DEPLOY_USER}@${DEPLOY_HOST}"
TMP_TAR="/tmp/directcall-linux.tar.gz"
TMP_CONFIG="/tmp/directcall-config.json"

echo "[deploy] Uploading artifact to ${REMOTE}:${TMP_TAR}"
scp "${SSH_OPTS[@]}" "${ARTIFACT_PATH}" "${REMOTE}:${TMP_TAR}"
echo "[deploy] Uploading config to ${REMOTE}:${TMP_CONFIG}"
scp "${SSH_OPTS[@]}" "${CONFIG_PATH}" "${REMOTE}:${TMP_CONFIG}"

echo "[deploy] Installing on remote host"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "mkdir -p '${DEPLOY_PATH}' && tar -xzf '${TMP_TAR}' -C '${DEPLOY_PATH}' --strip-components=1 && mv '${TMP_CONFIG}' '${DEPLOY_PATH}/config.talking-face.json' && chmod +x '${DEPLOY_PATH}/directcall' '${DEPLOY_PATH}/run-directcall.sh' && rm -f '${TMP_TAR}'"

echo "[deploy] Ensuring runtime certificate files exist"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "if [ ! -f '${DEPLOY_PATH}/cert.pem' ] || [ ! -f '${DEPLOY_PATH}/key.pem' ]; then openssl req -x509 -newkey rsa:4096 -keyout '${DEPLOY_PATH}/key.pem' -out '${DEPLOY_PATH}/cert.pem' -sha256 -days 3650 -nodes -subj '/C=US/ST=NA/L=NA/O=Wilddolphin/OU=DirectCall/CN=directcall'; fi"

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

[Install]
WantedBy=multi-user.target
EOF"

echo "[deploy] Reloading and restarting ${SERVICE_NAME}"
ssh "${SSH_OPTS[@]}" "${REMOTE}" "systemctl daemon-reload && systemctl enable ${SERVICE_NAME} && systemctl restart ${SERVICE_NAME} && systemctl --no-pager --full status ${SERVICE_NAME} | sed -n '1,30p'"

if [ "${ENABLE_SIGNAL_BRIDGE}" = "true" ]; then
  echo "[deploy] Installing signal bridge script"
  ssh "${SSH_OPTS[@]}" "${REMOTE}" "cat > '${DEPLOY_PATH}/bridge_signal_tcp.py' <<'PY'
#!/usr/bin/env python3
import json
import socket
import time
import urllib.parse
import urllib.request
from typing import Optional

SIGNAL_BASE = '${SIGNAL_BASE_URL}'
ROOM = '${BRIDGE_ROOM}'
CALLEE_HOST = '${CALLEE_HOST}'
CALLEE_PORT = ${CALLEE_PORT}
POLL_INTERVAL = 0.2
SOCKET_TIMEOUT = 0.2

sock: Optional[socket.socket] = None
recv_buf = b''


def signal_get(params: dict):
    q = urllib.parse.urlencode(params)
    with urllib.request.urlopen(f'{SIGNAL_BASE}?{q}', timeout=5) as r:
        return json.loads(r.read().decode('utf-8', 'replace'))


def signal_post(body: dict):
    data = json.dumps(body).encode('utf-8')
    req = urllib.request.Request(
        f'{SIGNAL_BASE}?action=post&role=callee&room={urllib.parse.quote(ROOM)}',
        data=data,
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        r.read()


def ensure_socket():
    global sock
    if sock is not None:
        return
    s = socket.create_connection((CALLEE_HOST, CALLEE_PORT), timeout=2)
    s.settimeout(SOCKET_TIMEOUT)
    sock = s
    send_line('HELLO')
    send_line('INVITE:{\"agent\":\"audio\",\"room_name\":\"%s\"}' % ROOM)


def close_socket():
    global sock, recv_buf
    if sock is not None:
        try:
            sock.close()
        except Exception:
            pass
    sock = None
    recv_buf = b''


def send_line(line: str):
    ensure_socket()
    assert sock is not None
    sock.sendall(line.encode('utf-8'))


def process_incoming_from_callee():
    global recv_buf
    if sock is None:
        return
    while True:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                close_socket()
                return
            recv_buf += chunk
        except socket.timeout:
            break
        except Exception:
            close_socket()
            return

    if not recv_buf:
        return

    text = recv_buf.decode('utf-8', 'replace')
    recv_buf = b''

    prefixes = ['ANSWER:', 'ICE:', 'LLAMA:', '200 OK', '486 Busy Here', '400 Bad Request', '480 Temporarily Unavailable', 'BYE']
    cursor = 0
    while cursor < len(text):
        next_pos = len(text)
        next_prefix = None
        for p in prefixes:
            i = text.find(p, cursor)
            if i != -1 and i < next_pos:
                next_pos = i
                next_prefix = p
        if next_prefix is None:
            break
        msg_start = next_pos
        msg_end = len(text)
        for p in prefixes:
            i = text.find(p, msg_start + len(next_prefix))
            if i != -1:
                msg_end = min(msg_end, i)
        msg = text[msg_start:msg_end].strip()
        cursor = msg_end
        if not msg:
            continue
        try:
            if msg.startswith('ANSWER:'):
                signal_post({'type': 'answer', 'sdp': msg[len('ANSWER:'):]})
            elif msg.startswith('ICE:'):
                payload = msg[len('ICE:'):]
                idx, cand = payload.split(':', 1)
                signal_post({'type': 'ice', 'candidate': {'candidate': cand, 'sdpMLineIndex': int(idx), 'sdpMid': '0'}})
            elif msg.startswith('LLAMA:'):
                signal_post({'type': 'llama', 'text': msg})
            else:
                signal_post({'type': 'status', 'status': msg})
        except Exception:
            pass


def handle_browser_messages(msgs):
    for m in msgs:
        t = m.get('type')
        if t == 'offer':
            sdp = m.get('sdp', '')
            if sdp:
                send_line('OFFER:' + sdp)
        elif t == 'ice':
            c = m.get('candidate') or {}
            cand = c.get('candidate') if isinstance(c, dict) else None
            idx = c.get('sdpMLineIndex', 0) if isinstance(c, dict) else 0
            if cand:
                send_line(f'ICE:{int(idx)}:{cand}')
        elif t == 'face':
            data = m.get('data', '')
            if data:
                send_line('FACE:' + data)
        elif t == 'hangup':
            try:
                send_line('BYE')
            finally:
                close_socket()


def main():
    try:
        signal_get({'action': 'reset', 'role': 'callee', 'room': ROOM})
    except Exception:
        pass

    while True:
        try:
            polled = signal_get({'action': 'poll', 'role': 'callee', 'room': ROOM})
            msgs = polled.get('messages', []) if isinstance(polled, dict) else []
            if msgs:
                handle_browser_messages(msgs)
            process_incoming_from_callee()
        except Exception:
            pass
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
