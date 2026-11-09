#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the RenderDoc replay tools (README.md) into out/renderdoc/bin: RenderDoc 1.46's Linux
# release (librenderdoc, the Android layer) and its source's replay headers are fetched into
# out/renderdoc first.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
here=tools/renderdoc
out=$PWD/out/renderdoc
version=1.46
mkdir -p "$out/bin"
if [[ ! -f $out/renderdoc_$version/lib/librenderdoc.so ]]; then
    curl -sfL -o "$out/rd.tar.gz" "https://renderdoc.org/stable/$version/renderdoc_$version.tar.gz"
    tar xzf "$out/rd.tar.gz" -C "$out"
    rm "$out/rd.tar.gz"
fi
if [[ ! -f $out/renderdoc-$version/renderdoc/api/replay/renderdoc_replay.h ]]; then
    curl -sfL -o "$out/src.tar.gz" "https://github.com/baldurk/renderdoc/archive/refs/tags/v$version.tar.gz"
    tar xzf "$out/src.tar.gz" -C "$out" --wildcards '*/renderdoc/api/replay/*'
    rm "$out/src.tar.gz"
fi
# the capture layer for BB_APK_LAYERS, out of RenderDoc's own Android package
layer=$out/layer/lib/arm64-v8a/libVkLayer_GLES_RenderDoc.so
if [[ ! -f $layer ]]; then
    unzip -q -o "$out/renderdoc_$version/share/renderdoc/plugins/android/org.renderdoc.renderdoccmd.arm64.apk" \
        lib/arm64-v8a/libVkLayer_GLES_RenderDoc.so -d "$out/layer"
fi
lib=$out/renderdoc_$version/lib
# -rdynamic: librenderdoc finds REPLAY_PROGRAM_MARKER in the program
for tool in rdremote rddebug rdhist rdviews rdpatch rdvs; do
    "${CXX:-g++}" -std=c++17 -O1 -rdynamic -DRENDERDOC_PLATFORM_LINUX -DRENDERDOC_WINDOWING_XLIB \
        -I"$out/renderdoc-$version/renderdoc/api/replay" "$here/$tool.cpp" \
        -L"$lib" -lrenderdoc -Wl,-rpath,"$lib" -o "$out/bin/$tool"
done
echo "Built $out/bin/{rdremote,rddebug,rdhist,rdviews,rdpatch,rdvs}; layer: $layer"
