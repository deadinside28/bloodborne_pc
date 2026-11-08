#!/usr/bin/env bash
# The GPU library (gpu/) for Android arm64-v8a with the NDK, against out/android/deps
# (tools/android_deps.sh): out/android/gpu/libbbgpu.so. No FFmpeg (movies off), no online module,
# no X11.
#   tools/android_gpu.sh
# Env: BB_NDK, BB_ANDROID_API (35).
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${BB_NDK:-$(ls -d "$HOME"/.local/opt/android-sdk/ndk/* | sort -V | tail -1)}
prefix=$PWD/out/android/deps
cmake -S gpu -B out/android/gpu -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-${BB_ANDROID_API:-35}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DPKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
    -DVulkan_INCLUDE_DIR="$prefix/include" -DBB_NET=OFF -DBB_LTO=OFF \
    -DCMAKE_CXX_FLAGS="-include cstdlib" \
    -DBB_FFMPEG=$([[ -f $prefix/lib/libavcodec.a ]] && echo ON || echo OFF)  # tools/android_ffmpeg.sh
cmake --build out/android/gpu --target bbgpu -- -k 0
