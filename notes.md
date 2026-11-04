# Working notes: Android / ARM64 port of bbport

Handoff for continuing on another machine (next: a gaming PC with an NVIDIA GPU). Branch
`recompiler` of `gitlab.com/NumNums01/bloodborne_pc` (private), on top of upstream bbport 0.7
(`github.com/deadinside28/bloodborne_pc`, commit `0fc2921`). Last updated 2026-11-04.

Longer documents: [docs/ANDROID_ARM64_PORT_PLAN.md](docs/ANDROID_ARM64_PORT_PLAN.md) (the plan,
with status per phase) and [recompiler/README.md](recompiler/README.md) (the recompiler: status
table, use, execution model, deviations, ordered to-do list).

## Where things stand

- **Recompiler** (`recompiler/`): x86-64 -> LLVM IR -> x86-64 / AArch64, ahead of time. Python
  front end (iced-x86), ~600 instruction semantics, jump tables, imports, x86 memory ordering.
  - Corpus: 144 functions pass in 54 configurations (x86-64 and AArch64 under QEMU), and on a
    real phone (Galaxy S25 Ultra) over adb.
  - The system's libz: identical output to the original on x86-64 and AArch64 (1,581 checks).
  - **The game** (`eboot.bin` only): 162,961 functions, 9.98M instructions lift in under a
    minute; 3 unsupported sites (junk decodes), 24 decode failures (upstream's analysis: 22),
    every direct call resolved; compiled for AArch64: 243 MiB of code (6.1x the original).
  - x86 memory ordering on the phone: 1.08x with `ldapr`/`stlr` + inline alignment check
    (default `--tso`), against 3.5x with barriers.
  - **Nothing recompiled from the game has run yet.**
- **Phone facts** (S25 Ultra, Snapdragon 8 Elite, Android 17): 39-bit user address space,
  4 KiB pages, LSE, LRCPC, LRCPC2, LSE2.
- **The game's address use** (bbport on PC, title + menus only): highest guest address
  `0x1152f20000` (~69 GiB), peak 5.3 GiB of guest memory. Fits the phone's 39 bits easily, so
  guest addresses can stay identity-mapped; re-measure in gameplay before relying on it.

## Moving to the NVIDIA PC

1. Clone (submodules come later from `build.sh`):
   ```bash
   git clone -b recompiler https://gitlab.com/NumNums01/bloodborne_pc.git && cd bloodborne_pc
   ```
   GitLab asks for a username and a token (any user name, the token as password). Use a token
   with only `read_repository` / `write_repository`; the one used so far is a broad `api` token
   in plain text on the old desktop, expiring 2026-11-20.
2. **The game dump is not in git** (and must never be: derived files stay in ignored `out/`
   folders). Copy `CUSA03173/` (30 GiB, 28,850 files, decrypted 1.09) next to the repository:
   `../CUSA03173`, which `run.sh` finds by default (`BB_GAME_DIR` overrides).
3. bbport's build dependencies on Arch (on another distro, the same list from `shell.nix`):
   ```bash
   sudo pacman -S --needed cmake ninja glslang sdl3 vulkan-headers vulkan-icd-loader boost fmt \
       robin-map miniz zydis nlohmann-json spirv-headers spirv-cross miniupnpc xxhash ffmpeg \
       openssl protobuf zlib nvidia-utils
   ```
   Two header-only libraries are not in Arch's repositories: put VulkanMemoryAllocator v3.5.0
   (`include/vk_mem_alloc.h`, also as `vma/vk_mem_alloc.h`) and xbyak v7.44 (`xbyak/*.h`) in
   `~/.local/include`, then build with
   ```bash
   CPATH=$HOME/.local/include CMAKE_INCLUDE_PATH=$HOME/.local/include bash build.sh
   ```
   (magic_enum is fetched by CMake itself.)
4. Run: `BB_SAVE_LOG=1 bash run.sh` (logs in `logs/`, frame statistics with `BB_FRAME_STATS=1`).
   bbport's main targets are AMD and NVIDIA on Linux; NVIDIA has the sparse binding the Intel
   laptop lacked, so the hang below should not occur there.
5. The recompiler: `python3 -m venv .venv-recomp && .venv-recomp/bin/pip install iced-x86
   pyelftools`; tests: `recompiler/harness/run.sh`, `run_zlib.sh` (need gcc, clang). AArch64
   replay needs Zig 0.13 and `qemu-aarch64-static` (`BB_ZIG`, `BB_QEMU`; defaults under
   `~/.local/opt`); phone runs: `recompiler/harness/device/run_device.sh` with adb.
6. On Windows instead: bbport builds natively there too (upstream README); the recompiler's Python
   parts run anywhere, its test harness is Linux-only (fork, `/proc`, ELF loading).

## Local changes to bbport itself (on this branch, not upstream)

- `b82b534` **upstream PR #416** (by AlexanderDzhoganov): weak definitions of
  `ffxVkShaderOverride` / `ffxVkJobMark`, without which 0.7 does not link from source (issue
  #404). Drop when upstream merges it.
- `36b5550` **integrated GPUs:** the guest-memory type searches skipped every memory type of an
  integrated Intel GPU (all device-local), so the game stopped at start. Worth sending upstream.

## Open problem on Intel integrated GPUs (found on Iris Plus, 2026-11-03)

Deterministic GPU hang (device lost) when the intro movie starts after *New Game*:

1. The movie reads a 256 KiB buffer (`0x1152ee0000+0x40010`) whose last 16 bytes lie in a 32 KiB
   piece of direct memory mapped just before (`0x1152f20000`).
2. bbport cannot bind it in place (two imported chunks) and its GPU-copy stitch path gives up,
   most likely because the new chunk is not imported yet (lazy import).
3. The fallback is the sparse arena (`GetArena`, `MemoryType::Sparse`): on a GPU without
   `sparseBinding` (Intel ANV here) nothing backs it (`vkQueueBindSparse: 0 calls`), the GPU reads
   unbacked memory and hangs. Log: `BB_BDA_LOG=1` shows the arena created right before the hang.

Likely the same as upstream issue #340 (Intel UHD 620). Fix idea, smallest first: for read-only
bindings, copy the bytes from guest memory (CPU-readable) into the stream buffer when sparse
binding is unavailable; or import the missing chunk on demand so the stitch works; and never
create a sparse arena on GPUs that cannot back it. Not implemented.

## Baseline on the NVIDIA PC (2026-11-04)

Fedora 44, RTX 3070, NVIDIA 615.71.09. Game dump at `~/Games/CUSA03173` (`BB_GAME_DIR`).

- Builds from source with the online module (`libbbnet20.so`). Fedora needs, in `~/.local`:
  Vulkan-Headers 1.4.357 (Fedora's 1.4.341 lacks `VK_VALVE_shader_mixed_float_dot_product`),
  VMA 3.5.0, xbyak 7.44, Zydis 4.1.1 + Zycore (static; Fedora's zydis-devel 5.0 has no CMake
  package), and a CMake config shim for Fedora's protobuf 3.19.6. Build/run with
  `CPATH=$HOME/.local/include CMAKE_INCLUDE_PATH=$HOME/.local/include`.
- Unmodified game: title, New Game, intro movie (H.264 via Fedora's ffmpeg-free -> libopenh264),
  character creation and early gameplay, no errors or device lost. Menus ~480 FPS, cinematic and
  gameplay ~77 FPS.
- Guest address peak through early gameplay (`out/va_peak.json`, 350 samples over ~12 min; no new peak after the first minutes of gameplay):
  user memory end `0x1189f40000` (~70 GiB), 6.6 GB peak, low region end `0x808718000`; within
  39 bits. Re-check after longer play (more areas loaded).
- Profile (`perf record -g -F 999`, 60 s of gameplay, `out/perf.data`; per-function totals in
  `out/hot_functions.json`, mapped with the eboot's FDE ranges): game code is 31 % of all
  samples (libbbgpu 41 %, NVIDIA driver 9 %, libc 10 %). Flat: 4,273 game functions sampled; the
  top 40 hold 32 % of game-code time, the top 200 hold 58 %. Hottest by self time: `0x76b150`
  (1.3 % of all samples), `0xe106d0`, `0x21b8710`, `0x2ba84a0`, `0x26adb90`, `0x26bc320`. Game-code
  time by thread: main 8.6 %, Havok workers 5.5 %, GXRenderThread + GXWorker:1-5 ~11 %, FMOD 1.7 %.
  Inclusive numbers from frame-pointer call chains are approximate (game code omits frame pointers).

## Next steps (in order)

1. ~~Baseline on the NVIDIA PC~~ (done 2026-11-04, above).
2. **Hybrid mode** (branch `hybrid`; milestone 1 done 2026-11-04: thunks and corpus tests, see
   recompiler/README.md "Hybrid execution"; next: `BB_AOT` in bbport with one hot function, then
   the top 40/200 from `out/hot_functions.json`, keeping functions whose live bytes differ from
   eboot.bin (patches, hooks; `fs`->`gs` prefixes aside) original): on x86-64 the original and the recompiled code can run in one process. Patch
   function entries (the way bbport's guest-code hooks do) to jump into recompiled functions for a
   chosen set, grow the set, and bisect when the game misbehaves; then run with no original code.
   Needs: the linked PRX modules lifted too (`scripts/link_modules.py` image), the game's 661 GOT
   imports bound to bbport's runtime, bbport's own code hooks as a recompiler input table, and C++
   exceptions (38 functions with handlers: the unwinder jumps into the middle of functions).
3. Multi-threaded tests of `lock` atomicity and of `--tso` ordering, on the phone.
4. Code size (6.1x): flags and register write-backs at calls and exits are the main cost.
5. Report the LLVM 23.1.4 AArch64 crash (`recompiler/harness/llvm-bug-store-merge-bswap16.ll`);
   `build_game.sh` works around it with `-mstrict-align` for the one affected shard.

## Tools (all in the repository)

| What | Where |
|---|---|
| Recompile, survey or build the game | `python -m recomp game EBOOT [--out DIR] [--report survey.json] [--abi] [--tso]`, `recompiler/harness/build_game.sh EBOOT [MODE] [TARGET]` |
| Corpus / libz differential tests (x86-64 + AArch64 QEMU) | `recompiler/harness/run.sh`, `run_zlib.sh` |
| Phone (adb) | `recompiler/harness/device/run_device.sh`, `va_probe.c` (address-space probe) |
| Guest address peak of a running bbport | `recompiler/harness/va_sampler.py OUT.json` |
| Drive the game's menus remotely (KDE Plasma 6, Wayland) | `recompiler/harness/desktop/bbkeys.py enter down wait:2 ...` (uinput keyboard, writable for the seat's user via logind; refuses unless the game window is active) and `focus_bb.sh` (KWin script) |
| Keep a desktop session unlocked while a run lasts | `systemd-inhibit --what=idle:sleep ... bash -c 'while pgrep -x bb-probe ...'`; unlock remotely with `loginctl unlock-session <session>` |
