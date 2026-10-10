#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Instala todas as dependências necessárias para compilar o BBPort no Ubuntu 22.04 LTS
set -euo pipefail

echo "======================================================================"
echo "    BBPort - Instalador de Dependências para Ubuntu 22.04 LTS"
echo "======================================================================"

SUDO=""
if [[ $EUID -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "[!] Este script precisa de privilégios root para instalar pacotes." >&2
        exit 1
    fi
fi

# 1. Repositórios e dependências básicas
echo -e "\n[*] [1/7] Instalando pacotes básicos e repositório de toolchains..."
$SUDO apt-get update
$SUDO apt-get install -y software-properties-common ca-certificates curl wget git pkg-config python3 python3-pip ninja-build nasm

$SUDO add-apt-repository -y ppa:ubuntu-toolchain-r/test
$SUDO apt-get update

# 2. GCC 13 e bibliotecas de desenvolvimento do sistema
echo -e "\n[*] [2/7] Instalando GCC 13 (C++23) e bibliotecas de desenvolvimento..."
$SUDO apt-get install -y gcc-13 g++-13 \
    libxxhash-dev \
    libx11-dev libxcb1-dev libwayland-dev libxext-dev \
    libasound2-dev libpulse-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libxss-dev libxxf86vm-dev libxkbcommon-dev libdrm-dev libgbm-dev libxtst-dev \
    libzstd-dev libgl1-mesa-dev libvulkan-dev glslang-tools

# Configura gcc-13 e g++-13 como padrão do sistema
$SUDO update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 --slave /usr/bin/g++ g++ /usr/bin/g++-13
$SUDO update-alternatives --set gcc /usr/bin/gcc-13

# 3. CMake moderno (>= 3.24), Vulkan-Headers e Boost moderno
echo -e "\n[*] [3/7] Instalando CMake moderno, Vulkan-Headers e Boost 1.84..."
pip3 install --no-cache-dir cmake

git clone --depth 1 --branch vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/Vulkan-Headers.git /tmp/vulkan-headers
cmake -G Ninja -S /tmp/vulkan-headers -B /tmp/vulkan-headers/build-usr -DCMAKE_INSTALL_PREFIX=/usr
$SUDO cmake --build /tmp/vulkan-headers/build-usr --target install
cmake -G Ninja -S /tmp/vulkan-headers -B /tmp/vulkan-headers/build-local -DCMAKE_INSTALL_PREFIX=/usr/local
$SUDO cmake --build /tmp/vulkan-headers/build-local --target install
rm -rf /tmp/vulkan-headers

curl --http1.1 -fSL --retry 3 https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz -o /tmp/boost.tar.gz
tar -xzf /tmp/boost.tar.gz -C /tmp boost_1_84_0/boost
rm -f /tmp/boost.tar.gz
$SUDO rm -rf /usr/include/boost /usr/local/include/boost
$SUDO mv /tmp/boost_1_84_0/boost /usr/local/include/boost
$SUDO ln -s /usr/local/include/boost /usr/include/boost
rm -rf /tmp/boost_1_84_0

# 4. SDL3 (compilação a partir da fonte oficial)
echo -e "\n[*] [4/7] Compilando e instalando SDL3..."
rm -rf /tmp/sdl3
git clone --depth 1 https://github.com/libsdl-org/SDL.git -b main /tmp/sdl3
cmake -G Ninja -S /tmp/sdl3 -B /tmp/sdl3/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DSDL_TEST_ENABLES=OFF -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XTEST=OFF
cmake --build /tmp/sdl3/build -j"$(nproc)"
$SUDO cmake --build /tmp/sdl3/build --target install
rm -rf /tmp/sdl3

# 5. FFmpeg minimal autocontido (H.264, AAC, MP4 para cutscenes do jogo)
echo -e "\n[*] [5/7] Compilando e instalando FFmpeg minimal..."
rm -rf /tmp/ffmpeg
git clone --depth 1 --branch n6.1 https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg
(
    cd /tmp/ffmpeg
    ./configure --prefix=/usr/local \
        --enable-shared --enable-pic --disable-static \
        --disable-programs --disable-doc --disable-everything \
        --enable-decoder=h264,aac,mp3,pcm_s16le \
        --enable-demuxer=mov,mp4,m4a,3gp,3g2,matroska \
        --enable-parser=h264,aac \
        --enable-protocol=file \
        --enable-swscale --enable-swresample
    make -j"$(nproc)"
    $SUDO make install
)
rm -rf /tmp/ffmpeg

# 6. fmt 10, Zydis e miniz
echo -e "\n[*] [6/7] Compilando e instalando fmt, Zydis e miniz..."
rm -rf /tmp/fmt /tmp/zydis /tmp/miniz

git clone --depth 1 --branch 10.2.1 https://github.com/fmtlib/fmt.git /tmp/fmt
cmake -G Ninja -S /tmp/fmt -B /tmp/fmt/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DFMT_TEST=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=ON
cmake --build /tmp/fmt/build -j"$(nproc)"
$SUDO cmake --build /tmp/fmt/build --target install
rm -rf /tmp/fmt

git clone --depth 1 --recursive https://github.com/zyantific/zydis.git /tmp/zydis
cmake -G Ninja -S /tmp/zydis/dependencies/zycore -B /tmp/zydis/dependencies/zycore/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DZYCORE_BUILD_SHARED_LIB=ON
cmake --build /tmp/zydis/dependencies/zycore/build -j"$(nproc)"
$SUDO cmake --build /tmp/zydis/dependencies/zycore/build --target install
cmake -G Ninja -S /tmp/zydis -B /tmp/zydis/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DZYDIS_BUILD_SHARED_LIB=ON
cmake --build /tmp/zydis/build -j"$(nproc)"
$SUDO cmake --build /tmp/zydis/build --target install
rm -rf /tmp/zydis

git clone --depth 1 --branch 3.1.2 https://github.com/richgel999/miniz.git /tmp/miniz
cmake -G Ninja -S /tmp/miniz -B /tmp/miniz/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_SHARED_LIBS=ON
cmake --build /tmp/miniz/build -j"$(nproc)"
$SUDO cmake --build /tmp/miniz/build --target install
rm -rf /tmp/miniz

# 7. Bibliotecas header-only: magic_enum, tsl-robin-map, VulkanMemoryAllocator
echo -e "\n[*] [7/7] Instalando magic_enum, tsl-robin-map e VulkanMemoryAllocator..."
rm -rf /tmp/magic_enum /tmp/robin_map /tmp/vma

git clone --depth 1 --branch v0.9.5 https://github.com/Neargye/magic_enum.git /tmp/magic_enum
$SUDO mkdir -p /usr/local/include/magic_enum
$SUDO cp /tmp/magic_enum/include/magic_enum/* /usr/local/include/magic_enum/
cmake -G Ninja -S /tmp/magic_enum -B /tmp/magic_enum/build -DCMAKE_INSTALL_PREFIX=/usr/local -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
$SUDO cmake --build /tmp/magic_enum/build --target install
rm -rf /tmp/magic_enum

git clone --depth 1 --branch v1.3.0 https://github.com/Tessil/robin-map.git /tmp/robin_map
cmake -G Ninja -S /tmp/robin_map -B /tmp/robin_map/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=/usr/local
$SUDO cmake --build /tmp/robin_map/build --target install
rm -rf /tmp/robin_map

git clone --depth 1 --branch v3.1.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git /tmp/vma
cmake -G Ninja -S /tmp/vma -B /tmp/vma/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=/usr/local
$SUDO cmake --build /tmp/vma/build --target install
rm -rf /tmp/vma

git clone --depth 1 https://github.com/herumi/xbyak.git /tmp/xbyak
cmake -G Ninja -S /tmp/xbyak -B /tmp/xbyak/build -DCMAKE_INSTALL_PREFIX=/usr/local
$SUDO cmake --build /tmp/xbyak/build --target install
rm -rf /tmp/xbyak

$SUDO ldconfig

echo -e "\n======================================================================"
echo "    TODAS AS DEPENDÊNCIAS FORAM INSTALADAS COM SUCESSO!"
echo "    Agora você pode rodar: python3 package.py"
echo "======================================================================"
