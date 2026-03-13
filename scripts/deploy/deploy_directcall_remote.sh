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

echo "[deploy] Done"
