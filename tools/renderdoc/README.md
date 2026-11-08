# RenderDoc on the phone

Capture a frame of bbport on an Android device and replay it **on the device**. The PC cannot
replay an Adreno capture (APIHardwareUnsupported), and qrenderdoc waits on its first-run
analytics question. These small tools drive librenderdoc's replay API through RenderDoc's
remote server over adb instead. This is how the black screen on Adreno was found: the display
shader's `DenormFlushToZero 32` (notes.md, 2026-11-08).

## Build

    bash tools/renderdoc/build.sh

This fetches RenderDoc 1.46 (the Linux release and the replay headers from its source) into
`out/renderdoc`, extracts the arm64 capture layer, and builds the tools into
`out/renderdoc/bin`.

## Capture

1. Build the APK with the layer packed in. Android did not search a layer app's own libraries,
   so the layer goes into bbport's library folder, which also makes the app debuggable:

       BB_APK_LAYERS=out/renderdoc/layer/lib/arm64-v8a/libVkLayer_GLES_RenderDoc.so bash tools/android_apk.sh

2. Turn the layer on for bbport:

       adb shell settings put global enable_gpu_debug_layers 1
       adb shell settings put global gpu_debug_app org.bbport
       adb shell settings put global gpu_debug_layers VK_LAYER_RENDERDOC_Capture

3. Start bbport with a trigger file. The app's `env` extra sets variables:

       adb shell am start -n org.bbport/.BbportActivity \
           --es env "BB_RENDERDOC_TRIGGER=/sdcard/Android/data/org.bbport/files/rdc/go"

   When the frame you want is on screen, create the file
   (`adb shell touch .../rdc/go`). The next frame is saved as `rdc/bbport_*.rdc`
   (BB_RENDERDOC_DIR picks another folder). Pull it with `adb pull`.

4. Afterwards, turn the layer off:

       adb shell settings delete global enable_gpu_debug_layers
       adb shell settings delete global gpu_debug_app
       adb shell settings delete global gpu_debug_layers

Capturing costs memory. A run under RenderDoc once failed at startup with ConstantRing: out of
device memory.

## Replay

Each tool connects to the first adb device and starts RenderDoc's remote server there.
RenderDoc installs its own `org.renderdoc.renderdoccmd.arm64` app for this; uninstall it when
you are done. Each tool then copies the capture over and replays it. Run `watch.sh` beside them:
it stops the server when the phone's available memory drops below 1.5 GB, or when a reading
fails.

    tools/renderdoc/watch.sh <serial> out/renderdoc/watch.log &

| Tool | Arguments | What it prints |
| --- | --- | --- |
| `rdremote` | `capture.rdc` | Every action, with the min and max of its first outputs, to find where a frame goes black |
| `rdhist` | `capture.rdc event x y` | Pixel history: every event that touched the pixel, with its result |
| `rddebug` | `capture.rdc event x y` | The event's fragment shader disassembly, then a debug trace of the pixel |
| `rdviews` | `capture.rdc event...` | The views each event writes and reads through, their formats, the textures' flags |
| `rdpatch` | `capture.rdc event x y dump.spv [variant.spv ...]` | Saves the event's fragment shader, then swaps each variant in and reads the pixel back |

To make variants for `rdpatch`, disassemble `dump.spv` with the NDK's `spirv-dis`
(`$ANDROID_NDK/shader-tools/*/spirv-dis`), edit the text, and reassemble it with `spirv-as`.
