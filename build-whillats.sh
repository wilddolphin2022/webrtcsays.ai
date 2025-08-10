#!/bin/sh

set -e

usage()
{
    echo 'usage: build-whisper [-d|-r|-c]
    where:
        -d to create a debug build default)
        -r to create a release build
        -c to clean the build artifacts'
}

clean()
{
    # Remove all possible artifact directories.
    cd ${WEBRTC_SRC_DIR}/modules/third_party/whillats
    echo "rm -rf build in modules/third_party/whillats"
    rm -rf build
}

WEBRTC_SRC_DIR="${PWD}/../src"
WHILLATS_DIR="${WEBRTC_SRC_DIR}/third_party//whillats"
WHILLATS_THIRD_PARTY_DIR="${WHILLATS_DIR}/third_party"

case "$(uname -s | tr '[:upper:]' '[:lower:]')" in
    linux)
        HOST_PLATFORM="linux"
        ;;
    msys*|mingw*)
        HOST_PLATFORM="windows"
        ;;
    darwin)
        HOST_PLATFORM="mac"
        ;;
    *)
        HOST_PLATFORM="unknown"
        ;;
esac

BUILD_TYPE=debug

while [ "$1" != "" ]; do
    case $1 in
        -d | --debug )
            BUILD_TYPE=debug
            ;;
        -r | --release )
            BUILD_TYPE=release
            ;;
        -c | --clean )
            clean
            exit
            ;;
        -h | --help )
            usage
            exit
            ;;
        * )
            usage
            exit 1
    esac
    shift
done

    echo "HOST_PLATFORM is ${HOST_PLATFORM}, BUILD_TYPE is ${BUILD_TYPE}, WEBRTC_SRC_DIR is ${WEBRTC_SRC_DIR}"

    cd ${WHILLATS_THIRD_PARTY_DIR}
    if [ ! -f ${WHILLATS_THIRD_PARTY_DIR}/whisper.cpp/CMakeLists.txt ]; then
    echo "cloning whisper.cpp to ${PWD}"
    git clone https://github.com/ggerganov/whisper.cpp
    fi

    if [ ! -f ${WHILLATS_THIRD_PARTY_DIR}/llama.cpp/CMakeLists.txt ]; then
    echo "cloning llama.cpp to ${PWD}"
    git clone https://github.com/ggerganov/llama.cpp
    fi

    if [ ! -f ${WHILLATS_THIRD_PARTY_DIR}/espeak-ng/CMakeLists.txt ]; then
    echo "cloning espeak-ng to ${PWD}"
    git clone https://github.com/espeak-ng/espeak-ng
    fi

    if [ ! -f ${WHILLATS_THIRD_PARTY_DIR}/pcaudiolib/Makefile.am ]; then
    echo "cloning pcaudiolib to ${PWD}"
    git clone https://github.com/espeak-ng/pcaudiolib
    fi

    cd ${WHILLATS_THIRD_PARTY_DIR}/whisper.cpp
    
    if [ "${HOST_PLATFORM}" = "linux" ]
    then
        sed -i 's/Wunreachable-code-break/Wunreachable-code/g' ggml/src/CMakeLists.txt 
        sed -i 's/Wunreachable-code-return/Wunreachable-code/g' ggml/src/CMakeLists.txt
    fi

    echo "building whisper.cpp"
    cmake -B build

    if [ "${BUILD_TYPE}" = "release" ]
        cmake --build build --config Release  
    then
        cmake --build build --config Debug  
    fi

    if [ "${HOST_PLATFORM}" = "linux" ]
    then
        echo "installing whisper.cpp"
        cd build; sudo make install; cd ..
    fi

    cd ${WHILLATS_THIRD_PARTY_DIR}/llama.cpp

    if [ "${HOST_PLATFORM}" = "linux" ]
    then
        sed -i 's/Wunreachable-code-break/Wunreachable-code/g' ggml/src/CMakeLists.txt 
        sed -i 's/Wunreachable-code-return/Wunreachable-code/g' ggml/src/CMakeLists.txt
    fi

    echo "building llama.cpp"
    cmake -B build

    if [ "${BUILD_TYPE}" = "release" ]
        cmake --build build --config Release  
    then
        cmake --build build --config Debug  
    fi

    if [ "${HOST_PLATFORM}" = "linux" ]
    then
        echo "installing llama.cpp"
        cd build; sudo make install; cd ..
    fi

    cd ${WHILLATS_THIRD_PARTY_DIR}/espeak-ng

    echo "building espeak-ng"

    cmake -B build

    if [ "${BUILD_TYPE}" = "release" ]
        cmake --build build --config Release  
    then
        cmake --build build --config Debug  
    fi

    if [ "${HOST_PLATFORM}" = "linux" ]
    then
        echo "installing espeak-ng"
        sudo make install
    fi

    echo "building pcaudio"

    cd ${WHILLATS_THIRD_PARTY_DIR}/pcaudiolib

    echo "building pcaudiolib"
    ./autogen.sh
    ./configure --with-pic
    make

    if [ "${HOST_PLATFORM}" = "mac" ]
    then
        ./libtool --mode=install cp src/libpcaudio.la  ${WHILLATS_THIRD_PARTY_DIR}/pcaudiolib/src/libpcaudio.dylib
    fi
