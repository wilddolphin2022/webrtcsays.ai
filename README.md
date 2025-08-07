# WebRTCsays.ai Build Instructions

## Getting the Code
To clone the repository and build the project, run:
```bash
git clone https://github.com/wilddolphin2022/webrtcsays.ai webrtcsays.ai
cd webrtcsays.ai
git checkout develop
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

### Required Steps for Speech-Enabled Builds
If you want to build with whillats (Whisper/Llama/TTS integration), you must:
1. Initialize and update submodules:
   ```bash
   git submodule update --init --recursive
   ```
2. Build the Whisper library:
   ```bash
   cd src/modules/third_party/whillats
   # For debug build:
   make debug
   # For release build:
   make release
   cd -
   ```
Run these steps before running the main build script with the `whillats` option.

### Usage
```bash
./build.sh [debug|release] [whillats]
```
- `debug` (default): Build with debug symbols and no optimizations
- `release`: Build with optimizations for production
- `whillats`: Enable advanced AI capabilities with Whisper + LLaMA + TTS integration (includes speech functionality)

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
- **Debug build with whillats:**
  ```bash
  ./build.sh debug whillats
  ```
- **Release build with whillats:**
  ```bash
  ./build.sh release whillats
  ```

### What do the AI options do?
- **`whillats`**: Enables advanced AI capabilities including both Whisper (speech-to-text) and LLaMA (language model) integration. This builds the complete AI pipeline with GPU acceleration support when available.
- If omitted, the build uses standard audio devices without AI features.

---

## Platform/Architecture Notes
- The script auto-detects your architecture (`x86_64`, `arm64`, etc.) and sets up sysroots/toolchains as needed.
- On ARM64 (aarch64), the script uses the system Clang and may build additional runtime libraries if missing.
- On x86_64, standard Chromium sysroots and toolchains are used.

### GPU Acceleration (CUDA)
- **CUDA Support**: The build system automatically detects NVIDIA CUDA and attempts to enable GPU acceleration for Whisper and LLaMA models.
- **Current Status**: Due to compatibility issues between CUDA 12.8 and glibc 2.41, the build currently uses **CPU-only mode** for maximum stability.
- **Performance**: CPU-only mode with OpenMP provides excellent performance for real-time speech processing.
- **Future CUDA**: Your RTX GPU is ready for CUDA when compatibility issues are resolved. See [CUDA.md](CUDA.md) for technical details and solutions.

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
- If the build fails, check the output for error messages and ensure you are on the correct branch (`develop`).
- **CUDA Issues**: If you see CUDA-related build errors, refer to [CUDA.md](CUDA.md) for detailed troubleshooting and compatibility information.
- **Math Function Errors**: Conflicts between CUDA 12.8 and glibc 2.41 are automatically handled by falling back to CPU-only mode.

---

## Advanced
- The script stores the last built commit in `last_build_commit.txt` to avoid unnecessary rebuilds.
- You can manually clean the build output by deleting the `src/out/` directory.

---

## Contact
For issues or questions, please open an issue on the project's GitHub page.
