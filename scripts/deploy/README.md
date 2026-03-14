# DirectCall deployment

This repo now includes:

- `scripts/ci/build_directcall_linux.sh` - builds and packages `directcall` for Linux.
- `scripts/deploy/deploy_directcall_remote.sh` - uploads and installs package over SSH.
- `.github/workflows/deploy-directcall.yml` - manual GitHub Actions deploy workflow.

## GitHub Actions secrets

Set these repository secrets:

- `DEPLOY_SSH_KEY` - private key contents for SSH access (for `root@217.77.3.65`).
- `DEPLOY_HOST` - `217.77.3.65`
- `DEPLOY_USER` - `root`
- `DEPLOY_PORT` - `22` (or your custom SSH port)
- `DIRECTCALL_CONFIG_JSON` - full runtime JSON config (recommended).

## Run from GitHub Actions

1. Open **Actions** -> **Deploy DirectCall**.
2. Click **Run workflow**.
3. Optional inputs:
   - `git_ref`: branch/tag/SHA (default `talkingface`)
   - `deploy_path`: default `/opt/directcall`
   - `service_name`: default `directcall`
   - `config_path`: fallback path in repo if `DIRECTCALL_CONFIG_JSON` is not set.
   - `enable_signal_bridge`: install/start `directcall-bridge` service (default `true`)
   - `bridge_room`: browser demo room routed to callee (default `testroom`)
   - `signal_base_url`: public signal endpoint (default `https://www.wilddolphin.us/signal.php`)

The workflow builds `dist/directcall-linux.tar.gz`, prepares `dist/directcall-config.json`, uploads both files, installs to the target path, writes a `systemd` service, and restarts it.

Service startup command is:

`/opt/directcall/run-directcall.sh --config /opt/directcall/config.talking-face.json`

If `/opt/directcall/cert.pem` or `/opt/directcall/key.pem` is missing, deploy script creates self-signed certs automatically.

When `enable_signal_bridge=true`, deploy also installs:

- `${deploy_path}/bridge_signal_tcp.py`
- `systemd` unit `directcall-bridge` (or custom `BRIDGE_SERVICE_NAME`)

This bridge forwards browser messages from `signal.php` room traffic to the native callee on `127.0.0.1:3456`.

## Run locally with your current SSH command

```bash
cd /Users/ykiryanov/Personal/Wilddolphin/webrtcsays.ai/src
chmod +x scripts/ci/build_directcall_linux.sh scripts/deploy/deploy_directcall_remote.sh

# Build package on this machine (Linux recommended)
scripts/ci/build_directcall_linux.sh

# Deploy using your key and host
SSH_KEY_PATH="$HOME/.ssh/id_wd2025" \
DEPLOY_HOST="217.77.3.65" \
DEPLOY_USER="root" \
DEPLOY_PORT="22" \
DEPLOY_PATH="/opt/directcall" \
SERVICE_NAME="directcall" \
scripts/deploy/deploy_directcall_remote.sh \
  dist/directcall-linux.tar.gz \
  scripts/deploy/directcall.config.example.json
```
