#!/usr/bin/env bash
# The GPU library's dependencies for Android (arm64-v8a), built with the NDK into one prefix:
# out/android/deps (lib/, include/, lib/cmake/, lib/pkgconfig/). Sources are cloned (shallow, the
# versions the PC build uses) into out/android/src. Header-only ones (Boost, robin-map, VMA) are
# copied from this machine; Vulkan comes with the NDK.
#   tools/android_deps.sh
# Env: BB_NDK (default: the newest under ~/.local/opt/android-sdk/ndk), BB_ANDROID_API (35).
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${BB_NDK:-$(ls -d "$HOME"/.local/opt/android-sdk/ndk/* | sort -V | tail -1)}
api=${BB_ANDROID_API:-35}
src=$PWD/out/android/src
prefix=$PWD/out/android/deps
mkdir -p "$src" "$prefix/include" "$prefix/lib/pkgconfig"
cmake_android=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a
    -DANDROID_PLATFORM="android-$api" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
clone() {  # url tag dir
    [[ -d $src/$3 ]] || git clone --depth 1 --branch "$2" --recurse-submodules --shallow-submodules "$1" "$src/$3"
}
build() {  # dir [cmake options]
    local d=$1; shift
    cmake -S "$src/$d" -B "$src/$d/build-android" "${cmake_android[@]}" "$@" > "$src/$d.log" 2>&1 &&
        cmake --build "$src/$d/build-android" >> "$src/$d.log" 2>&1 &&
        cmake --install "$src/$d/build-android" >> "$src/$d.log" 2>&1 ||
        { echo "FAILED $d (log: $src/$d.log)"; grep -m5 -i error "$src/$d.log"; exit 1; }
    echo "built $d"
}
clone https://github.com/libsdl-org/SDL.git release-3.4.18 SDL
build SDL -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
clone https://github.com/fmtlib/fmt.git 11.2.0 fmt
build fmt -DFMT_TEST=OFF -DFMT_DOC=OFF -DCMAKE_CXX_FLAGS="-include cstdlib"  # 11.2 misses it for the NDK's libc++
clone https://github.com/zyantific/zydis.git v4.1.1 zydis
build zydis/dependencies/zycore -DZYCORE_BUILD_SHARED_LIB=OFF  # Zydis' CMake package needs it installed
build zydis -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF
clone https://github.com/Cyan4973/xxHash.git v0.8.3 xxhash
build xxhash/cmake_unofficial -DXXHASH_BUILD_XXHSUM=OFF -DBUILD_SHARED_LIBS=OFF
# header-only, as this machine has them
cp -r /usr/include/boost "$prefix/include/"
cp -r /usr/include/tsl "$prefix/include/"
mkdir -p "$prefix/lib/cmake"
cp -r /usr/share/cmake/tsl-robin-map "$prefix/lib/cmake/" 2>/dev/null || cp -r /usr/lib64/cmake/tsl-robin-map "$prefix/lib/cmake/"
cp "$HOME/.local/include/vk_mem_alloc.h" "$prefix/include/"
mkdir -p "$prefix/include/vma" && cp "$HOME/.local/include/vk_mem_alloc.h" "$prefix/include/vma/"
echo "deps in $prefix"
