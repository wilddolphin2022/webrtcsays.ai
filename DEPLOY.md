# Deployment Guide

## Workflows

| Workflow | Purpose | Target |
|----------|---------|--------|
| **Deploy DirectCall** | Full build + deploy to CPU server | 217.77.3.65 (root) |
| **Deploy DirectCall (GPU)** | Full build + deploy to GPU server | 104.171.203.12 (ubuntu) |
| **Build DirectCall Only** | Build binary, optionally deploy | Artifact upload |
| **Build Whillats** | Build AI library standalone | Artifact upload |

## What Deploy Does

### Phase 1: CI Build (GitHub Actions ubuntu-22.04, ~20 min)

1. Checkout `takingface` branch with submodules (includes whillats)
2. `gclient sync` — fetch WebRTC third_party dependencies
3. Build whillats library (cmake, CPU-only on CI)
4. Build WebRTC + directcall (gn gen + ninja, ~3830 files)
5. Package `directcall`, `libdirect.so`, shared libs → `directcall-linux.tar.gz`
6. Prepare SSH key and runtime config JSON

### Phase 2: Deploy to Remote Host

1. **Preflight** (non-root only): `sudo chown` deploy dirs for deploy user
2. **Upload** tar.gz + config via scp
3. **Extract** binary + libs to `/opt/directcall/`
4. **TLS certs**: Generate self-signed if not present
5. **espeak-ng**: Install + symlink data to `/usr/local/share/espeak-ng-data`
6. **Download AI models** (if not present):
   - `ggml-small.bin` — Whisper STT (466 MB)
   - `Qwen2.5-1.5B-Instruct-Q4_K_M.gguf` — Small LLM for CPU (1.1 GB)
   - `Qwen3.5-9B-Q3_K_M.gguf` — Large LLM for GPU (4.4 GB)
7. **coturn TURN server** (CPU deploy only, skipped on GPU):
   - Install coturn, auto-detect server IP
   - Configure ports 3478 (UDP/TCP) + 5349 (TLS)
   - Credentials: `webrtcsays.ai` / `wilddolphin`
8. **Build whillats on target** (`talkingface` branch):
   - Auto-detects CUDA (`nvcc` present → `GGML_CUDA=ON`)
   - Builds `libwhillats.so` natively (avoids SIGILL from cross-compiled binaries)
   - Copies libwhillats + libggml-cuda + all ggml/llama shared libs
   - Copies espeak-ng-data from build deps
9. **Download StyleTTS2 ONNX models** (if not present, ~315 MB):
   - `bert_encoder.onnx`, `plbert_simp.onnx`, `final_simp.onnx`, `ref_s.bin`, `ref_p.bin`
10. **Write systemd units** (`directcall.service` + `directcall-bridge.service`):
    - `LD_LIBRARY_PATH` includes `/usr/local/cuda/lib64`
    - `STYLETTS2_MODEL_DIR` → `/opt/whillats/trained_models`
    - `ESPEAK_DATA_PATH` → `/opt/whillats/build/bin/Release/espeak-ng-data`
    - `GGML_BACKEND_DL_SEARCH_PATH` → `/opt/directcall/lib`
11. **Write `run-directcall.sh`** with `stdbuf -oL` (unbuffered logs) + CUDA lib path
12. **Start directcall** service
13. **Install signal bridge** (`bridge_signal_tcp.py` v2):
    - AsyncTCPSocket 2-byte length-prefix framing
    - Protocol-aware message reordering (OFFER before ICE)
    - Fresh TCP session per OFFER
    - Room reset on startup
14. **Start bridge** service
15. **Cleanup**: Remove build artifacts from CI runner

## Secrets Required

| Secret | CPU Deploy | GPU Deploy | Description |
|--------|-----------|-----------|-------------|
| `DEPLOY_SSH_KEY` | ✅ | | SSH private key for CPU server |
| `DEPLOY_HOST` | ✅ | | CPU server IP |
| `DEPLOY_USER` | ✅ | | CPU server user (root) |
| `DEPLOY_PORT` | ✅ | | CPU server SSH port |
| `DEPLOY_SSH_KEY_GPU` | | ✅ | SSH private key for GPU server |
| `DEPLOY_HOST_GPU` | | ✅ | GPU server IP |
| `DEPLOY_USER_GPU` | | ✅ | GPU server user (ubuntu) |
| `DIRECTCALL_CONFIG_JSON` | ✅ | ✅ | Runtime config (optional, falls back to example) |
| `HF_TOKEN` | ✅ | ✅ | HuggingFace token for model downloads |

## Architecture

```
Browser → signal.php (wilddolphin.us) ← bridge_signal_tcp.py → directcall (TCP :3456)
                                                                    ↓
                                                              Whisper STT
                                                                    ↓
                                                              Qwen LLM (CPU or CUDA)
                                                                    ↓
                                                              StyleTTS2 TTS
                                                                    ↓
                                                              WebRTC audio/video → Browser

TURN relay: coturn on 217.77.3.65 (ports 3478, 5349)
```

## Config JSON Fields

```json
{
  "mode": "callee",
  "address": "0.0.0.0:3456",
  "whisper_model": "/opt/models/ggml-small.bin",
  "llama_model": "/opt/models/Qwen3.5-9B-Q3_K_M.gguf",
  "whisper_threads": 4,
  "llama_threads": 8,
  "tts_threads": 4,
  "turns": [
    ["turn:217.77.3.65:3478?transport=udp", "webrtcsays.ai", "wilddolphin"],
    ["turn:217.77.3.65:5349?transport=tcp", "webrtcsays.ai", "wilddolphin"]
  ],
  "talking_face": "/opt/directcall/RobotPhoneLogo.jpeg"
}
```

Thread counts: set to `0` for auto (all cores). On shared CPU servers, split cores
between components (e.g. whisper=4, llama=6, tts=2 for 8 cores).

## Triggering a Deploy

```bash
# CPU server
gh workflow run deploy-directcall.yml --ref takingface -f git_ref=takingface

# GPU server
gh workflow run deploy-directcall-gpu.yml --ref takingface -f git_ref=takingface
```
