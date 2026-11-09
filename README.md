THIS PROJECT IS NOT RELATED TO SHADPS4. Do not send questions about it to the shadPS4 project.

# Bloodborne on Android (bbport for AArch64)

This is an Android port of [bbport](https://github.com/deadinside28/bloodborne_pc), the native
port of *Bloodborne* for PlayStation 4 (CUSA03173, game version 1.09). On a PC bbport runs the
game's x86-64 code directly; a phone's CPU cannot, so here the whole game executable is
**statically recompiled** ahead of time, x86-64 → LLVM IR → AArch64, into one library
(`libbbaot.so`). The phone then runs:

- the recompiled game code (`libbbaot.so`, built on a PC from your own game dump);
- bbport's runtime, which replaces the PS4 system libraries (memory, threads, files, audio,
  pad, saves);
- bbport's renderer, which translates the game's GPU work and shaders to Vulkan, here on the
  phone's GPU (tested: Adreno 830).

> **No game files are included**, and none may be: the recompiled library and the prepared game
> files are derived from your own dump. You need a decrypted dump of Bloodborne with update
> 1.09. This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

**Status: experimental.** Tested on one device, a Samsung Galaxy S25 Ultra (Snapdragon 8 Elite,
Adreno 830, 12 GB). The title screen, the character creator and loading a save into the game
work, with sound and touch or gamepad controls. Known problems:

- **Low frame rate in game** (about 15 FPS measured at the first area), worse once the phone
  heats up and Android lowers the GPU clock (down to 160 MHz was seen).
- **Flicker in game:** some frames are about twice as bright, as if the game's exposure value
  were wrong for that frame; and parts of characters can vanish for a single frame. Both are
  being investigated.
- **Memory:** the game needs most of the phone's free memory. With other apps open Android's
  low-memory killer can close it. Close other apps first.
- An occasional crash in the game's own code (a missing object at offset 0x38) has been seen
  in game; it also happens rarely on the PC.

Progress, measurements and open questions are logged in [notes.md](notes.md).

## What you need

- An **Android phone**, arm64, Android 13 (API 33) or newer, with Vulkan 1.3; 12 GB of RAM is
  what it was tested with. Your game dump copied to it (about 30 GB).
- A **Linux x86-64 PC as the build machine**. The Android build is made there, and part of it
  needs bbport's PC build: the game image, the patch file and the recompiler's input come from
  running the game once on the PC. The PC is used only to build and to test; it is not a way to
  play.
- On that PC: the [Android SDK and NDK](https://developer.android.com/studio) (the scripts look
  under `~/.local/opt/android-sdk`; `BB_NDK` names another NDK), a JDK (`JAVA_HOME`, default
  `~/.local/opt/jdk-*`), the PC build's tools (GCC, CMake, Ninja, Python 3, glslang, SDL3, Vulkan
  headers and the libraries in `shell.nix`; with [Nix](https://nixos.org) they all come from
  `shell.nix`), clang with the AArch64 target, `patchelf`, and Python packages for the recompiler
  (below). About 40 GB free disk for the library build.
- Your decrypted game dump, version 1.09 (the base game alone, 1.00, does not start; a dumped
  update is copied over the base game).

## Building

All steps run on the Linux PC, from the repository's root.

### 1. bbport for the PC (the build machine's part)

```bash
git clone --recursive https://github.com/NumNums01/bloodborne_android.git bbport && cd bbport
bash build.sh                                   # out/bb-probe, out/gpu/libbbgpu.so
```

### 2. The game image and the patch file, at the phone's render size

The game's render targets are patched at start; the phone uses 1280×720. Running the game on the
PC once with that size writes `out/boot-linked.bin`, `out/content.bin` and `out/patches.bin`,
and `BB_AOT_DUMP` saves the game's code as it runs after the port's patches (the recompiler's
input). Close the game once the dump is written (the log says
`AOT: the game's code as it runs written to ...`).

```bash
export BB_GAME_DIR=/path/to/CUSA03173
BB_RENDER_RES=1280x720 BB_AOT_DUMP=$PWD/out/aot/live720-plain.bin bash run.sh
BB_RENDER_RES=1280x720 BB_FRAME_STATS=1 BB_AOT_DUMP=$PWD/out/aot/live720-stats.bin bash run.sh
```

The second dump covers the configuration with frame statistics on (it installs a hook of its
own). The size must match the phone's `BB_RENDER_RES` (`src/android_main.c`): the patch file,
the dumps and the library are made for one size together, and the game refuses a library whose
functions differ from its code.

### 3. The recompiled game library (AArch64)

The recompiler needs a Python environment next to the repository:

```bash
python3 -m venv ../.venv-recomp && ../.venv-recomp/bin/pip install iced-x86 pyelftools
```

Then lift and compile every function of the game (the list is every function address of the
linked game image, one per line: `recompiler/out/aot/sites6/list.txt` in the builds so far, a
copy is kept as `list.txt` next to each library built):

```bash
BB_AOT_TARGET=aarch64 BB_RECOMP_MODE=abi BB_RECOMP_JOBS=8 \
  bash recompiler/harness/build_aot.sh $BB_GAME_DIR/eboot.bin LIST \
  out/aot/live720-plain.bin,out/aot/live720-stats.bin sites11v720
```

This writes `recompiler/out/aot/sites11v720-aarch64/libbbaot.so` (about 210 MiB; about 7 minutes
with 8 jobs, each compile job takes 1–3 GiB of RAM). Never commit it or the files it is made
from: they are derived from your dump. How the recompiler works:
[recompiler/README.md](recompiler/README.md).

### 4. The Android libraries and the app

```bash
bash tools/android_deps.sh       # the GPU library's dependencies, built with the NDK (once)
bash tools/android_ffmpeg.sh     # optional: FFmpeg for the game's movies (once)
bash tools/android_gpu.sh        # out/android/gpu/libbbgpu.so
bash tools/android_apk.sh recompiler/out/aot/sites11v720-aarch64/libbbaot.so
```

The result is `out/android/bbport.apk` (arm64-v8a, debug-signed). For debugging,
`BB_APK_VALIDATION=<libVkLayer_khronos_validation.so>` packs Khronos' validation layer and makes
the app debuggable (`adb shell run-as org.bbport` can then read its files).

### 5. The game's files for the phone

```bash
bash tools/android_prepare.sh $BB_GAME_DIR
```

This makes `out/android/bbport-data/` with `boot-linked.bin`, `content.bin`, `patches.bin` and
the dump in `app0/`. Copy that folder to the phone (USB file transfer or a card).

## Installing and playing

1. Install the app: `adb install -r out/android/bbport.apk` (or copy the APK to the phone and
   open it).
2. Start **bbport**. On the first start it asks for the folder from step 5 and copies it into
   its own storage, with progress (it needs about 2 GB free beyond the game; an interrupted copy
   resumes).
3. Close other apps before playing: the game uses most of the phone's memory.

**Controls.** On-screen touch controls are shown while no gamepad is connected: a floating
movement stick on the left, a camera stick on the right, the DualShock 4's buttons, and
**Menu**, which opens the port's settings. A Bluetooth or USB gamepad works too; the touch
controls then hide. Rotate the character in the creator with the camera stick.

**Defaults on Android** (`src/android_main.c`; the port's menu changes most of them while
playing):

| Setting | Default | Why |
|---|---|---|
| Render size | 1280×720 (`BB_RENDER_RES`, needs the matching library) | GPU work and memory |
| Upscaler | FSR 1 (`BB_UPSCALER=fsr1`), to the 1920×1080 frame | FSR 3's context took over 5 s and enough memory to have the game killed |
| Preset | Native (`BB_UPSCALE_PRESET=0`, `BB_PATCH_PRESET=0`) | the targets are 720p already |
| Texture quality | Half (`BB_TEXTURE_QUALITY=half`) | memory |
| Object motion vectors | Off (`BB_OBJECT_MOTION=0`) | the GPU was lost in the character creator with them |
| Dynamic state at every draw | On (`BB_DYNAMIC_STATE_ALWAYS=1`) | depth writes were lost otherwise |

Saves, the shader cache and logs stay in the app's storage
(`/sdcard/Android/data/org.bbport/files`).

## Testing and debugging

**On the phone.** The game's log is in Android's log: `adb logcat --pid=$(adb shell pidof
org.bbport)`. Environment variables are passed at start:

```bash
adb shell am start -n org.bbport/.BbportActivity --es env "'BB_FPS_PRINT=1 BB_UPSCALER=off'"
```

Two diagnostics are armed in every build and do nothing until their trigger file appears:
`touch /sdcard/Android/data/org.bbport/files/user/pd.trigger` saves the next 16 frames as shown
(`user/pdump/`), `.../user/cap.trigger` records the next frame's passes as text (`user/capg/`).
`out/android/drive.sh` runs the game to the character creator by a pad file, watching the
phone's free memory and stopping the game below 1.5 GB. RenderDoc can capture and replay frames
on the phone itself: [tools/renderdoc/README.md](tools/renderdoc/README.md).

**On the Linux PC (testing only).** bbport's PC build runs the same renderer and runtime, which
makes it the quickest place to check a change for breakage. `BB_GUEST_MEMORY=copy
BB_OBJECT_MOTION=0` makes it behave like the phone (the GPU copies the game's memory instead of
reading it in place; the CPU copy-shader path is on). Phone-only behaviour cannot be tested
there: Android's hardware buffers, the Adreno driver, the low-memory killer and the phone's
speed. The Android emulator is no substitute either: its GPU allocator refuses the hardware
buffers the port uses and its Vulkan lacks extensions the renderer needs.

Every `BB_*` variable, with what it does: [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md).

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader and the runtime that replaces the PS4 system libraries; `android_main.c` (the app's entry and its defaults) |
| `gpu/` | Renderer library: command processor, shader translator, renderer, caches, upscalers, menu, touch controls |
| `recompiler/` | The static recompiler: x86-64 → LLVM IR → AArch64 ([README](recompiler/README.md)) |
| `android/` | The Android app's Java part (SDL activity, first-start setup) |
| `scripts/` | Offline preparation of the game image, module linking, patch compiler |
| `tools/` | `android_*.sh` build scripts, RenderDoc tools, test helpers |
| `patches/` | Community patches for Bloodborne |
| `docs/`, `notes.md` | Design notes and the log of this port's work |

## Credits and licenses

This port is based on **bbport** ([deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc))
and keeps its license: the **GNU GPL v2 or later** ([LICENSE](LICENSE)); it contains code from
shadPS4 (GPL-2.0-or-later). Reusing the code: keep its copyright and license notices
(`SPDX-FileCopyrightText: Copyright 2026 bbport contributors` at the top of its files) and
[NOTICE](NOTICE), and say where the code comes from. Third-party components keep their licenses:
[shadPS4](https://github.com/shadps4-emu/shadPS4) (GPL-2.0+: the renderer and shader translator
were derived from it and rewritten), [sirit](https://github.com/shadps4-emu/sirit),
[half](https://half.sourceforge.net/), [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by
FireBurn (MIT), AMD FidelityFX SDK (MIT), [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)
(MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [SDL](https://libsdl.org) (zlib),
[FFmpeg](https://ffmpeg.org) (LGPL, optional), DejaVu fonts. Game patches by Kyo, Lance McDonald,
auser1337, illusion, emoose and other community members (`patches/Bloodborne.xml`).

bbport's contributors are listed in [docs/CONTRIBUTORS_0.7.md](docs/CONTRIBUTORS_0.7.md). Related
project: [ducvd89/Bloodborne-Android](https://github.com/ducvd89/Bloodborne-Android), another
Android fork of bbport.
