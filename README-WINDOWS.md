# The Windows port (windows-port branch)

This branch carries the native Windows port of bbport, synced with upstream **0.51**
(memory module, the reused-command-buffer ("PM4 type 0") fix, native mouse look, no
online/offline screen, the settings-in-bbport.ini remapping system).

Boots, plays, saves. Off on Windows for now (Linux-only upstream features): shadNet
online play, the DLSS bridge, the game's own System-menu settings pages — the Insert
overlay covers the settings.

## Building (MSYS2 CLANG64)

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64` and update it (`pacman -Syu`,
   again until nothing is left to do).
2. Install the toolchain and libraries (same list as the README's Windows section):

   ```
   pacman -S --needed git mingw-w64-clang-x86_64-{clang,lld,libc++,cmake,ninja,pkgconf,python,sdl3,boost,fmt,glslang,spirv-cross,spirv-headers,vulkan-headers,vulkan-loader,vulkan-memory-allocator,xxhash,zydis,robin-map,ffmpeg}
   ```

3. Build in the CLANG64 shell (`C:\msys64\clang64.exe`):

   ```
   bash build.sh
   ```

   The result is `out\bb-probe.exe` (plus `out\bb-gpu-capabilities.exe`). `build.sh`
   rebuilds only what changed; `BB_PREBUILT=1` skips the build check entirely.

4. Run with the bundled/any Python:

   ```
   python scripts\run_game.py --game-dir <folder with eboot.bin>
   ```

## Launching without a console

`tools/windows-launcher/launcher.cs` builds a small launcher exe (settings mirror of
`run.bat`, handy as a Steam target for Steam Input). It only needs the C# compiler
that ships with Windows:

```
%CSC% -nologo -target:exe -out:"Bloodborne PC.exe" launcher.cs
```

## The Windows porting layer

- `src/win32_*.c` — the Win32 loader/runtime (guest address space as one placeholder,
  section-backed pool, fault handling).
- `gpu/shim/bbport_ucontext.h` — fault handlers written against Linux `ucontext_t`
  read the vectored handler's `EXCEPTION_POINTERS` through the same gregs shape.
- `gpu/shim/bbport_posix_shim.h` — `dladdr`/`mmap`/`mprotect`/`gettid`/`backtrace`
  stand-ins over the Win32 APIs.
- `#ifdef _WIN32` branches throughout `src/` (SRWLOCK locks, `win_mem_*` pool
  mapping, save-file copy-on-write through host handles) and the Windows keep of
  upstream's recovery (`BbRecoverBuf` register snapshot + `runtime_setjmp`).

## Keyboard, mouse, gamepad (bbport.ini)

Everything is remappable in `bbport.ini` without a rebuild:

```
mouse_look=1        # the window takes the mouse on a click; F1 releases it
mouse_sensitivity=1.0
key.cross=space     # any input name: cross circle square triangle l1 r1 l2 r2 l3 r3
key.circle=left shift   #   options touchpad up down left right move_* look_*
walk_key=left ctrl  # held, the move keys tilt the stick only walk_percent % (silent walk)
walk_percent=35
pad.r1=a            # gamepad buttons by SDL names
```

Mouse defaults: left R1 (light attack), right L2 (gun), middle R3 (lock-on),
X1 R2, X2 L1, wheel up/down = quick items. Multi-word key names keep their
inner space ("left shift", "left ctrl"): "leftshift" does not parse, and the
binding is silently lost.

**Asian game dumps** (CUSA01363 &co.) swap the whole pair — Circle is interact +
menu confirm, Cross is dodge/sprint + menu cancel — so swap the keys to keep
LShift dodging and Space interacting:

```
key.cross=lshift
key.circle=space
```

License: GPL-2.0-or-later, as the project. The game itself is not part of this
repository; use your own dump.
