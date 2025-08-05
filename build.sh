#!/bin/bash

# Exit on any error
set -e

# Clean up old directories if they exist, but keep src
echo "Cleaning up old directories..."
if [ -f ".gclient" ]; then
    echo "Removing existing .gclient file..."
    rm -f .gclient
fi

# Ensure we're on the avatar branch
echo "Switching to development branch..."
git fetch origin develop 
git checkout develop 

# Verify we're on the correct branch
echo "Current commit: $(git rev-parse HEAD)"
echo "Current branch: $(git branch --show-current)"

# Store the current commit hash for comparison
CURRENT_COMMIT=$(git rev-parse HEAD)
LAST_BUILD_COMMIT_FILE="last_build_commit.txt"
ORIGIN_URL=$(git remote get-url origin)

# Create or update .gclient file in the root directory with 'name': 'src'
cat > .gclient << EOF
solutions = [
  {
    "managed": False,
    "name": "src",
    "url": "$ORIGIN_URL",
    "custom_deps": {},
    "deps_file": "DEPS",
  },
]
EOF

# Check if src directory exists and is initialized
if [ -d "src" ] && [ -d "src/build" ] && [ -f "src/.git/HEAD" ]; then
    echo "Skipping gclient sync as src directory already exists and appears initialized."
else
    echo "Running gclient sync to initialize src directory..."
    gclient sync --nohooks --no-history --shallow
    if [ ! -d "src/build" ]; then
        echo "ERROR: src/build directory missing after gclient sync. Check your DEPS file or sync process."
        exit 1
    fi
fi

# Copy .vpython3 to src directory
cp .vpython3 src/ || { echo "WARNING: .vpython3 not found in root, build may fail if dependencies are incorrect."; }

# Navigate to src directory
cd src

git fetch origin develop 
git checkout develop 

# Detect platform architecture for sysroot
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        SYSROOT_ARCH=amd64
        ;;
    aarch64|arm64)
        SYSROOT_ARCH=arm64
        ;;
    *)
        echo "Unsupported architecture: $ARCH"
        SYSROOT_ARCH=""
        ;;
esac

if [ -n "$SYSROOT_ARCH" ] && [ -f build/linux/sysroot_scripts/install-sysroot.py ]; then
    echo "Installing sysroot for architecture: $SYSROOT_ARCH"
    python3 build/linux/sysroot_scripts/install-sysroot.py --arch=$SYSROOT_ARCH
else
    echo "Sysroot install script not found or not required for this architecture. Skipping sysroot installation."
fi

# For ARM64, use system Clang and ensure required packages are installed
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    echo "Detected ARM64 platform. Using system Clang and ensuring required packages are installed..."
    apt-get update
    apt-get install -y clang lld || {
        echo "ERROR: Failed to install clang/lld. Please check your APT sources and keys.";
        exit 1;
    }
    CLANG_PATH=$(which clang || true)
    if [ -z "$CLANG_PATH" ]; then
        echo "ERROR: clang not found after installation. Please install clang manually."
        exit 1
    fi
    echo "System Clang found at $CLANG_PATH"
    # Set GN args to use system Clang and specify clang_version 14
    EXTRA_ARGS="clang_base_path=\"/usr\" clang_use_chrome_plugins=false target_cpu=\"arm64\" use_custom_libcxx=false clang_version=\"14\""
    echo "Skipping prebuilt Clang download for ARM64."
    # Build and install libclang_rt.builtins.a if missing
    BUILTINS_PATH="/usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/libclang_rt.builtins.a"
    if [ ! -f "$BUILTINS_PATH" ]; then
        echo "libclang_rt.builtins.a not found, building from source..."
        TMP_LLVM_DIR="/tmp/llvm-project"
        if [ ! -d "$TMP_LLVM_DIR" ]; then
            git clone --depth=1 https://github.com/llvm/llvm-project.git "$TMP_LLVM_DIR"
        fi
        cd "$TMP_LLVM_DIR"
        rm -rf build-builtin
        mkdir build-builtin
        cd build-builtin
        cmake -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DLLVM_ENABLE_PROJECTS="compiler-rt" \
          -DLLVM_TARGETS_TO_BUILD="AArch64" \
          -DCOMPILER_RT_BUILD_BUILTINS=ON \
          -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
          -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
          -DLLVM_INCLUDE_TESTS=OFF \
          -DCMAKE_INSTALL_PREFIX=/usr \
          ../llvm
        ninja
        FOUND_BUILTIN=$(find . -name 'libclang_rt.builtins.a' | head -n1)
        if [ -f "$FOUND_BUILTIN" ]; then
            sudo mkdir -p /usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/
            sudo cp "$FOUND_BUILTIN" /usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/
            echo "libclang_rt.builtins.a installed to /usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/"
        else
            echo "ERROR: libclang_rt.builtins.a was not built successfully."
            exit 1
        fi
        cd -
    fi
    # Skip update.py
else
    python3 tools/clang/scripts/update.py
fi

# Parse script parameters
BUILD_TYPE="debug"
ENABLE_SPEECH="false"
if [ $# -ge 1 ]; then
    if [[ "$1" == "debug" || "$1" == "release" ]]; then
        BUILD_TYPE="$1"
    else
        echo "Unknown build type: $1. Use 'debug' or 'release'."
        exit 1
    fi
fi
if [ $# -ge 2 ]; then
    if [[ "$2" == "speech" || "$2" == "enable_speech" ]]; then
        ENABLE_SPEECH="true"
    fi
fi

# If speech is enabled, update submodules and build Whisper library
if [ "$ENABLE_SPEECH" = "true" ]; then
    echo "Speech option enabled: updating submodules and building Whisper library..."
    git submodule update --init --recursive
    if [ -d "src/modules/third_party/whillats" ]; then
        pushd src/modules/third_party/whillats
        if [ "$BUILD_TYPE" = "debug" ]; then
            make debug
        else
            make release
        fi
        popd
    else
        echo "ERROR: src/modules/third_party/whillats directory not found."
        exit 1
    fi
fi

# Dynamically detect src directory
if [ -d "src" ]; then
    SRC_DIR="src"
else
    SRC_DIR="."
fi

# Set binary path and build dir based on build type and SRC_DIR
if [ "$BUILD_TYPE" = "debug" ]; then
    BINARY_PATH="$SRC_DIR/out/debug/direct_app"
    BUILD_DIR="$SRC_DIR/out/debug"
else
    BINARY_PATH="$SRC_DIR/out/release/direct_app"
    BUILD_DIR="$SRC_DIR/out/release"
fi

# Clean build output directory for ARM64 to avoid stale references to Clang 20
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    echo "Cleaning build output directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Check if binary already exists and if code has updates
BINARY_PATH="out/debug/direct_app"
LAST_BUILD_COMMIT=""
if [ -f "../$LAST_BUILD_COMMIT_FILE" ]; then
    LAST_BUILD_COMMIT=$(cat ../$LAST_BUILD_COMMIT_FILE)
fi

# Check if binary exists and is executable
if [ -f "$BINARY_PATH" ] && [ -x "$BINARY_PATH" ]; then
    # Test if binary can run (e.g., by checking version or help output)
    if "$BINARY_PATH" --help >/dev/null 2>&1; then
        echo "Binary at $BINARY_PATH is runnable. Cleaning up directories except src..."
        cd ..
        for dir in */ ; do
            if [ "$dir" != "src/" ]; then
                echo "Removing $dir..."
                rm -rf "$dir"
            fi
        done
        cd src
    else
        echo "Binary at $BINARY_PATH exists but is not runnable. Skipping cleanup of other directories."
    fi
else
    echo "Binary at $BINARY_PATH does not exist or is not executable. Skipping cleanup of other directories."
fi

# Parse script parameters
BUILD_TYPE="debug"
ENABLE_SPEECH="false"
if [ $# -ge 1 ]; then
    if [[ "$1" == "debug" || "$1" == "release" ]]; then
        BUILD_TYPE="$1"
    else
        echo "Unknown build type: $1. Use 'debug' or 'release'."
        exit 1
    fi
fi
if [ $# -ge 2 ]; then
    if [[ "$2" == "speech" || "$2" == "enable_speech" ]]; then
        ENABLE_SPEECH="true"
    fi
fi

if [ "$BUILD_TYPE" = "debug" ]; then
    echo "Building WebRTC project (debug, speech: $ENABLE_SPEECH)..."
    (cd $SRC_DIR && gn gen out/debug --args="is_debug=true rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=$ENABLE_SPEECH $EXTRA_ARGS")
    (cd $SRC_DIR && ninja -C out/$BUILD_TYPE direct_app)
    echo "Debug build completed."
else
    echo "Building WebRTC project (release, speech: $ENABLE_SPEECH)..."
    (cd $SRC_DIR && gn gen out/release --args="is_debug=false rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=$ENABLE_SPEECH $EXTRA_ARGS")
    (cd $SRC_DIR && ninja -C out/$BUILD_TYPE direct_app)
    echo "Release build completed."
fi

# Store the current commit hash as the last built commit
echo "$CURRENT_COMMIT" > ../$LAST_BUILD_COMMIT_FILE

if [ -f "cert.pem" ] && [ -f "key.pem" ]; then
    echo "Certificates already exist, skipping creation."
else
    # Creating certificates
    openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -sha256 -days 3650 -nodes -subj "/C=US/ST=CA/L=SanFrancisco/O=Acme/OU=Development/CN=WebRTCsays.ai"    
fi

echo "Running binary to show help..."
$BINARY_PATH --help

#echo "Running binary in callee mode to test signaling..."    
#$BINARY_PATH --mode=callee  --webrtc_cert_path=cert.pem  --webrtc_key_path=key.pem --user_name=Slim 3.93.50.189:3456
