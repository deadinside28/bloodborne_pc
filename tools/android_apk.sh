#!/usr/bin/env bash
# bbport's Android app: out/android/bbport.apk (arm64-v8a, debug-signed). SDL's activity
# (out/android/src/SDL, from tools/android_deps.sh) runs bb-probe built as libmain.so
# (src/android_main.c); the GPU library (tools/android_gpu.sh) and the recompiled game library
# (build_aot.sh, BB_AOT_TARGET=aarch64) come with it. The game's files go to the app's external
# files folder (tools/android_push.sh).
#   tools/android_apk.sh [LIBRARY]   (default recompiler/out/aot/sites11-aarch64/libbbaot.so)
# Env: BB_NDK, BB_ANDROID_API (35), JAVA_HOME (default ~/.local/opt/jdk-*).
set -euo pipefail
cd "$(dirname "$0")/.."
lib=$(readlink -f "${1:-recompiler/out/aot/sites11-aarch64/libbbaot.so}")
sdk=$HOME/.local/opt/android-sdk
ndk=${BB_NDK:-$(ls -d "$sdk"/ndk/* | sort -V | tail -1)}
api=${BB_ANDROID_API:-35}
tools=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
jar=$sdk/platforms/android-$api/android.jar
export JAVA_HOME=${JAVA_HOME:-$(ls -d "$HOME"/.local/opt/jdk-* | sort -V | tail -1)}
export PATH=$JAVA_HOME/bin:$PATH
cc=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$api-clang
out=out/android
deps=$out/deps
work=$out/apk
rm -rf "$work"; mkdir -p "$work/lib/arm64-v8a" "$work/classes" "$out/obj" "$out/include"
[[ -d $out/include/SDL3 ]] || cp -r /usr/include/SDL3 "$out/include/"

# libmain.so: bb-probe (probe.c's main as bb_probe_main) and the runtime, needing libbbgpu.so
for source in third_party/LibAtrac9/C/src/*.c; do
    "$cc" -std=c99 -O2 -fPIC -w -c "$source" -o "$out/obj/atrac9_$(basename "${source%.c}").o" &
done
wait
"$cc" -std=c11 -O2 -g -fPIC -Wall -Wextra -Werror -pthread -I"$out/include" -I. -Isrc -shared \
    -Dmain=bb_probe_main src/probe.c src/runtime*.c src/vulkan_smoke.c recompiler/harness/recomp_hybrid.c \
    src/android_main.c "$out"/obj/atrac9_*.o -L"$out/gpu" -lbbgpu -L"$deps/lib" -lSDL3 -llog -landroid -lm -ldl \
    -o "$work/lib/arm64-v8a/libmain.so"
cp "$out/gpu/libbbgpu.so" "$deps/lib/libSDL3.so" "$work/lib/arm64-v8a/"
cp "$lib" "$work/lib/arm64-v8a/libbbaot.so"
# The game library resolves the runtime's entry points (__recomp_dispatch...) in libmain.so: in an
# app's namespace only libraries it needs are searched, so it names libmain.so (patchelf, from its
# PyPI wheel: BB_PATCHELF, default out/android/tools/patchelf).
patchelf=${BB_PATCHELF:-out/android/tools/patchelf}
"$patchelf" --add-needed libmain.so "$work/lib/arm64-v8a/libbbaot.so"
# BB_APK_VALIDATION=<libVkLayer_khronos_validation.so> (Khronos' android-binaries, arm64-v8a): the
# layer packed and the app debuggable, so Android loads it from the app's library folder:
#   adb shell settings put global enable_gpu_debug_layers 1
#   adb shell settings put global gpu_debug_app org.bbport
#   adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
# (and `settings delete global ...` for the three afterwards). Messages go to logcat (VALIDATION).
manifest=android/AndroidManifest.xml
if [[ -n ${BB_APK_VALIDATION:-} ]]; then
    cp "$BB_APK_VALIDATION" "$work/lib/arm64-v8a/libVkLayer_khronos_validation.so"
    manifest=$work/AndroidManifest.xml
    sed 's|android:allowBackup="false">|android:allowBackup="false"\n        android:debuggable="true">|' \
        android/AndroidManifest.xml > "$manifest"
fi

# Java: SDL's activity and ours
"$JAVA_HOME/bin/javac" -nowarn -source 17 -target 17 -cp "$jar" -d "$work/classes" \
    "$out"/src/SDL/android-project/app/src/main/java/org/libsdl/app/*.java android/src/org/bbport/*.java
"$tools/d8" --release --min-api 33 --lib "$jar" --output "$work" $(find "$work/classes" -name '*.class')

# the package: manifest, dex, native libraries; aligned and signed
"$tools/aapt2" link --manifest "$manifest" -I "$jar" -o "$work/base.apk"
(cd "$work" && cp base.apk unsigned.apk && zip -q unsigned.apk classes.dex && zip -q -r unsigned.apk lib)
"$tools/zipalign" -P 16 -f 4 "$work/unsigned.apk" "$work/aligned.apk"
keystore=$out/debug.keystore
[[ -f $keystore ]] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$keystore" -storepass android -keypass android \
    -alias bbport -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=bbport debug" >/dev/null 2>&1
"$tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --out "$out/bbport.apk" "$work/aligned.apk"
echo "$out/bbport.apk: $(( $(stat -c %s "$out/bbport.apk") / 1048576 )) MiB"
