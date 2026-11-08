#!/usr/bin/env bash
# FFmpeg for the Android build (arm64-v8a): the game's movies (MP4: H.264 video, AAC audio)
# through platform/avplayer. A minimal static build into out/android/deps, linked into
# libbbgpu.so (no versioned shared libraries in the APK); tools/android_gpu.sh then builds with
# BB_FFMPEG=ON when it finds it.
#   tools/android_ffmpeg.sh
# Env: BB_NDK (default: the newest under ~/.local/opt/android-sdk/ndk), BB_ANDROID_API (35).
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${BB_NDK:-$(ls -d "$HOME"/.local/opt/android-sdk/ndk/* | sort -V | tail -1)}
api=${BB_ANDROID_API:-35}
src=$PWD/out/android/src/ffmpeg
prefix=$PWD/out/android/deps
bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
# FFmpeg 8.0: the API generation of the PC build (libavcodec 62)
[[ -d $src ]] || git clone --depth 1 --branch n8.0 https://github.com/FFmpeg/FFmpeg.git "$src"
build=$src/build-android
mkdir -p "$build"
cd "$build"
"$src/configure" --prefix="$prefix" --target-os=android --arch=aarch64 --cpu=armv8-a \
    --enable-cross-compile --cc="$bin/aarch64-linux-android$api-clang" \
    --cxx="$bin/aarch64-linux-android$api-clang++" --ar="$bin/llvm-ar" --nm="$bin/llvm-nm" \
    --ranlib="$bin/llvm-ranlib" --strip="$bin/llvm-strip" --pkg-config=false \
    --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
    --disable-everything --disable-network --disable-avdevice --disable-avfilter \
    --disable-zlib --disable-iconv --disable-autodetect \
    --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
    --enable-demuxer=mov --enable-decoder=h264,aac --enable-parser=h264,aac --enable-protocol=file \
    --extra-cflags="-O2" > "$src.configure.log" 2>&1 ||
    { echo "FAILED configure (log: $src.configure.log, $build/ffbuild/config.log)"; exit 1; }
make -j"$(nproc)" > "$src.build.log" 2>&1 && make install >> "$src.build.log" 2>&1 ||
    { echo "FAILED build (log: $src.build.log)"; grep -m5 -i error "$src.build.log"; exit 1; }
ls "$prefix"/lib/libav*.a "$prefix"/lib/libsw*.a
