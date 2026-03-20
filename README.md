# WebRTCsays.ai Build Instructions

## Getting the Code
To clone the repository and build the project, run:
```bash
git clone https://github.com/wilddolphin2025/webrtcsays.ai webrtcsays.ai
cd webrtcsays.ai
git checkout talkingface   # primary development branch (also: develop, main, …)
# Build (choose one of the following):
./build.sh release
./build.sh debug
./build.sh release whillats
./build.sh debug whillats
```

## Overview
WebRTCsays.ai is a project that brings AI-powered speech capabilities to WebRTC. This guide explains how to build the project using the provided `build.sh` script, including all available build options and platform notes.

---

## Prerequisites
- **Linux (x86_64, arm64/aarch64) or macOS**
- **Python 3**
- **depot_tools** ([installation guide](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/html/depot_tools_tutorial.html))
- **C++ build tools** (e.g., clang, ninja, cmake, make, etc.)
- **OpenSSL** (for certificate generation)
- **CUDA Toolkit 12.8+ (Optional)** - For GPU acceleration of Whisper/LLaMA models. See [CUDA.md](CUDA.md) for compatibility details.

### Install depot_tools
```bash
# Clone depot_tools and add to PATH
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
export PATH=~/depot_tools:$PATH
```

---

## Building WebRTCsays.ai

All builds are managed by the `build.sh` script in the project root. The script will:
- Set up the source tree and dependencies
- Configure the build for your platform/architecture
- Build the `directcall` binary
- Optionally enable AI speech features
- Generate self-signed certificates if needed

### Usage
```text
./build.sh [-b REF | --branch REF | --branch=REF] [--whillats-accel MODE] [debug|release|ios] [whillats]
```

| Argument / env | Purpose |
|----------------|---------|
| `debug` / `release` | Debug symbols vs optimized build (default if omitted: `debug` for host). |
| `ios` | iOS build; optional second arg `debug` or `release`. |
| `whillats` | Enable Whisper + LLaMA + TTS (whillats) in-tree; **host whillats GGML backend is CPU by default** (see below). |
| `-b REF`, `--branch REF`, `--branch=REF` | Git ref for the **`src/`** checkout only; outer clone branch unchanged. |
| `SRC_BRANCH=REF` | Same as `-b` if no flag on the command line. |
| `--whillats-accel MODE` | `cpu` (default) · `cuda` · `metal` (macOS) · `auto` (legacy: Metal on mac, else CUDA if `nvcc`). |
| `WHILLATS_ACCEL` | Same as `--whillats-accel` when unset on the CLI. |
| `WHILLATS_CUDA_ARCH` | Optional for `cuda` / `auto` CUDA path, e.g. `80-real` (avoids relying on `nvidia-smi`). |
| `BUILD_RM_ROOT_CIPD=1` | Delete repo-root `.cipd` before sync (large; re-downloaded next sync). |

Run `./build.sh` with an invalid first token (e.g. `./build.sh help`) to print the full in-script usage and **Notes**.

#### Examples
- **Debug build (default):**
  ```bash
  ./build.sh
  # or
  ./build.sh debug
  ```
- **Release build:**
  ```bash
  ./build.sh release
  ```
- **Debug build with whillats (CPU GGML, default):**
  ```bash
  ./build.sh debug whillats
  ```
- **Release with whillats on CUDA:**
  ```bash
  WHILLATS_ACCEL=cuda ./build.sh release whillats
  # or
  ./build.sh --whillats-accel=cuda release whillats
  ```
- **macOS Metal for whillats:**
  ```bash
  ./build.sh --whillats-accel=metal release whillats
  # or legacy auto-detect:
  WHILLATS_ACCEL=auto ./build.sh release whillats
  ```
- **Explicit `src/` ref (e.g. CI or pinned branch):**
  ```bash
  ./build.sh -b talkingface release whillats
  ```
- **Release iOS build with whillats:**
  ```bash
  ./build.sh ios release whillats
  ```
- **Debug iOS build without whillats:**
  ```bash
  ./build.sh ios debug
  ```

### What do the AI options do?
- **`whillats`**: Enables Whisper, LLaMA, and TTS integration built in-tree. The **WebRTC** side links against whillats headers/libs; **GGML** acceleration for whillats itself is **CPU unless** you set `WHILLATS_ACCEL` / `--whillats-accel` to `cuda`, `metal`, or `auto`.
- If `whillats` is omitted, the build uses standard audio devices without that AI stack.

### build.sh behavior (notes)
- **Working directory:** The script **`cd`s to the directory that contains `build.sh`** (repo root), not your current shell directory.
- **Layout:** Writes `.gclient` and runs **`gclient sync`** when `src/` is missing or incomplete; checkout and **GN/ninja** build use **`src/`**.
- **Root cleanup:** Removes **untracked** gclient “spill” directories at the repo root (`api/`, `third_party/`, `out/`, …) when **git does not track** files under them—so you do not keep a duplicate WebRTC tree next to `src/`. **Tracked** trees are never deleted.
- **Python:** Prefers **3.12** or **3.11** for depot_tools; warns on newer Python.
- **Whillats CMake:** Uses a clean `build/` directory each time; does **not** use `cmake --fresh` (works on **CMake before 3.24**, e.g. Ubuntu 22.04).
- **After a successful host build:** Runs **`directcall --help`** (skipped for iOS).
- **`last_build_commit.txt`:** Written at the **repo root** after a completed build, recording the **outer** repo commit that was built.

---

## Platform/Architecture Notes
- The script auto-detects your architecture (`x86_64`, `arm64`, etc.) and sets up sysroots/toolchains as needed.
- On ARM64 (aarch64), the script uses the system Clang and may build additional runtime libraries if missing.
- On x86_64, standard Chromium sysroots and toolchains are used.

### GPU acceleration (whillats / GGML)
- **Default:** **`WHILLATS_ACCEL=cpu`** — whillats builds **CPU-only** (no implicit CUDA even if `nvcc` is installed). Good for CI, laptops, and stable smoke builds.
- **CUDA:** Set **`WHILLATS_ACCEL=cuda`** (and ensure **`nvcc`** is on `PATH`). Optionally set **`WHILLATS_CUDA_ARCH`** (e.g. `80-real`) for a fixed architecture.
- **Metal (macOS):** **`WHILLATS_ACCEL=metal`**.
- **Legacy behavior:** **`WHILLATS_ACCEL=auto`** — Metal on macOS, else CUDA if `nvcc` exists, else CPU.
- **System / toolchain issues:** See [CUDA.md](CUDA.md) for CUDA versions, glibc, and troubleshooting.

---

## Output
- The main binary is built as:
  - `src/out/debug/directcall` (for debug)
  - `src/out/release/directcall` (for release)
- Self-signed certificates (`cert.pem`, `key.pem`) are generated in `src/` if not present.

---

## Running the Application
After building, the script will attempt to run the binary automatically. You can also run it manually:
```bash
# Show help
src/out/debug/directcall --help

# Example: Run as callee
src/out/debug/directcall --mode=callee 127.0.0.1:3456 --encryption --webrtc_cert_path=src/cert.pem --webrtc_key_path=src/key.pem

# Example: Run as caller
src/out/debug/directcall --mode=caller 127.0.0.1:3456 --encryption --webrtc_cert_path=src/cert.pem --webrtc_key_path=src/key.pem

# With Whisper enabled
src/out/debug/directcall --mode=callee 127.0.0.1:3456 --whisper --encryption
src/out/debug/directcall --mode=caller 127.0.0.1:3456 --whisper --encryption
```

---

## Using config.json
The `config.json` file allows you to predefine all runtime options for WebRTCsays.ai, such as mode, address, encryption, model paths, and more. This is useful for repeatable setups or automation.

To use a configuration file, pass it to the application with the `--config` flag:

```bash
src/out/debug/directcall --config config.json
```

You can still override individual options on the command line if needed. The application will load settings from `config.json` and use them as defaults unless overridden.

**Note:** The self-signed certificates (`cert.pem` and `key.pem`) are automatically created in the `src/` directory if they do not exist. Make sure your config paths point to `src/cert.pem` and `src/key.pem`.

---

## Example config.json
Below is an example `config.json` for running WebRTCsays.ai. Adjust paths and options as needed for your environment. (No passwords or sensitive data are included.)

```json
{
    "mode": "callee",
    "address": "3.93.50.189:3456",
    "user_name": "Slim",
    "room_name": "room101",
    "encryption": true,
    "whisper": true,
    "llama": false,
    "video": true,
    "bonjour": false,
    "language": "en",
    "whisper_model": "${HOME}/Public/models/ggml-small.bin",
    "llama_model": "${HOME}/Public/models/llava-llama-3-8b-v1_1-f16.gguf",
    "llava_mmproj": "${HOME}/Public/models/llava-llama-3-8b-v1_1-mmproj-f16.gguf",
    "webrtc_cert_path": "src/cert.pem",
    "webrtc_key_path": "src/key.pem",
    "webrtc_speech_initial_playout_wav": "",
    "turns": [
        ["turn:3.93.50.189:5349?transport=udp", "<username>", "<password>"]
    ],
    "camera": "0,640x480@30",
    "whispers": {
        "en": ["Hello, how are you?", "Please describe the image."],
        "zh": ["你好，你好吗？", "请描述图片。"]
    }
}
```

---

## config.json Field Descriptions
Below is an explanation of each field in the example config.json:

- **mode**: Operation mode for the app. Usually "callee" or "caller".
- **address**: The IP address and port to connect to or listen on (e.g., "3.93.50.189:3456").
- **user_name**: Your display/user name in the session.
- **room_name**: (Optional) Room identifier for multi-user or group sessions.
- **encryption**: Set to `true` to enable end-to-end encryption using the provided cert/key.
- **whisper**: Set to `true` to enable Whisper AI speech-to-text integration.
- **llama**: Set to `true` to enable LLaMA language model features (if supported).
- **video**: Set to `true` to enable video streaming; `false` for audio-only.
- **bonjour**: Set to `true` to enable Bonjour/zeroconf service discovery.
- **language**: Language code for speech recognition (e.g., "en", "zh").
- **whisper_model**: Path to the Whisper model file (e.g., "${HOME}/Public/models/ggml-small.bin").
- **llama_model**: Path to the LLaMA model file (if used).
- **llava_mmproj**: Path to the LLaVA multimodal projection model (if used).
- **webrtc_cert_path**: Path to the TLS certificate file (should be "src/cert.pem").
- **webrtc_key_path**: Path to the TLS private key file (should be "src/key.pem").
- **webrtc_speech_initial_playout_wav**: (Optional) Path to a WAV file to play at session start.
- **turns**: List of TURN server entries for NAT traversal. Each entry is a list: [URL, username, credential].
- **camera**: Camera device and settings (e.g., "0,640x480@30" for device 0 at 640x480, 30fps. NOTE: This works on Linux).
- **whispers**: Predefined prompt phrases for Whisper, organized by language code.

Adjust these fields as needed for your deployment and use case.

---

## Troubleshooting
- If you encounter missing dependencies, ensure all prerequisites are installed.
- For Linux/ARM64, the script will attempt to build missing runtime libraries if needed.
- If the build fails, check the log and branch: use **`talkingface`** (or the branch you intend); run **`./build.sh help`** for usage.
- **`CMake Error: Unknown argument --fresh`:** Use a **`build.sh`** that includes the fix (no `--fresh`); upgrade CMake to 3.24+ if you use `--fresh` elsewhere.
- **CUDA / whillats:** Use **`WHILLATS_ACCEL=cuda`** only when you want GPU; default **CPU** avoids accidental heavy CUDA builds. See [CUDA.md](CUDA.md).
- **Disk space:** Full **`gclient sync`** + whillats + WebRTC needs many GB; free space or set **`BUILD_RM_ROOT_CIPD=1`** if you need to reclaim `.cipd`.

---

## Advanced
- **`last_build_commit.txt`** at the repo root records the outer-repo commit after a **completed** build (used to reason about incremental rebuilds).
- Manually clean outputs: delete **`src/out/`** (and whillats **`src/modules/third_party/whillats/build`** if rebuilding whillats).

---

## Contact
For issues or questions, please open an issue on the project's GitHub page.
