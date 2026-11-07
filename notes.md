# Working notes: Android / ARM64 port of bbport

Handoff for continuing on another machine (next: a gaming PC with an NVIDIA GPU). Branch
`recompiler` of `gitlab.com/NumNums01/bloodborne_pc` (private), on top of upstream bbport 0.7
(`github.com/deadinside28/bloodborne_pc`, commit `0fc2921`). Last updated 2026-11-07.

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
2. **Hybrid mode** (branch `hybrid`, recompiler/README.md "Hybrid execution"). Done 2026-11-04:
   thunks and corpus tests; `BB_AOT` in bbport; the 198 hottest functions recompiled run in
   gameplay (two bugs found on the way: hoisted loads -> `--volatile`; unresolved jumps borrowed
   `[rsp]` -> `__recomp_dispatch_jump`). Recipe: `BB_AOT_DUMP=$PWD/out/aot/live.bin bash run.sh`
   (close it once written), `recompiler/harness/build_aot.sh EBOOT LIST out/aot/live.bin NAME`,
   `BB_AOT=$PWD/recompiler/out/aot/NAME/libbbaot.so bash run.sh`. Also done: the top 1,000 and
   all 4,273 sampled functions (4,034 swapped in) in gameplay; setjmp/longjmp found and handled
   (callers original, longjmp past recompiled frames resynchronised). Gameplay 37 FPS vs ~80:
   crossings and default-mode code (README "Cost"). Then the whole eboot: 160,854 of 162,959
   functions recompiled (14,953 for recompiled callers only: under 5 bytes or jumped into),
   2,103 original (2,001 setjmp/longjmp callers, 81 changed by bbport, 21 CFG errors); a 1.1 GB
   library built in 12.5 min; gameplay 44 FPS. Calls back per 90 s: 293M into bbport (imports),
   208M into the linked modules (libc.prx: not lifted yet), ~55M into the eboot. Then the
   modules lifted too (163,510 of 165,773 functions), light calls into bbport, and `--abi`:
   **gameplay 70 FPS** (~80 unmodified). Build: `BB_RECOMP_MODE=abi recompiler/harness/build_aot.sh
   EBOOT out/aot/all_linked.txt out/aot/live.bin,out/aot/live-stats.bin linked-abi` (6.4 min,
   511 MiB; IR and objects deleted as it goes: a kept full build is ~14 GB). Then longjmp
   callers recompiled (only setjmp callers stay): **165,383 of 165,771 functions, gameplay 79
   FPS** (as unmodified). Original: 154 with unsupported instructions (x87 in libc mostly), 130
   setjmp callers, 81 changed by bbport, 21 CFG errors, setjmp/longjmp themselves. Next: zero
   original code (those, and ~2M jumps back per run), then AArch64. Then (2026-11-04 afternoon): the
   allocator and cloth writer (runtime_aot_guest_pc for the write tracking), std/memmove, and
   setjmp in recompiled code (__recomp_setjmp + libc's longjmp replaced): **165,518 functions,
   255 original** (151 x87/unsupported in libc, 81 changed by bbport, 21 CFG errors, the 2
   setjmp/longjmp); 5M calls back per 3 min; same FPS and frame pacing as unmodified measured
   side by side (61 vs 62; the PC itself varied between 63 and 81 over the day). Then CFG
   recovery (calls that never return found from undecodable bytes after them; jump tables cut at
   the next table) and the x87 environment (fnstenv/fldenv/fnstcw/fldcw/fnstsw/fnclex/fninit
   through __recomp_x87): **165,554 recompiled, 219 original** (135 x87 maths in libc, 81
   changed by bbport, setjmp/longjmp, 1 CFG error); in gameplay the calls into original code
   go only to functions bbport changes. Then functions bbport only byte-patches lifted from the
   patched bytes (37), x87 run natively through __recomp_x87_insn, cmps and cmpxchg16b:
   **165,722 recompiled, 51 original** (44 with bbport's hooks, 4 unsupported, setjmp/longjmp,
   1 CFG error). Then an x87 emulator for AArch64 (`recompiler/harness/recomp_x87_emu.h`): all 394
   forms the lifter passes through match the hardware bit for bit (x87_fuzz.c, 79M runs), and
   the x87 corpus passes emulated on x86-64 and under QEMU. Then bbport's hooks as recompiler
   input (guest sites, `gpu/shim/bbport_guest_sites.h`): all 56 sites of the default settings
   (57 with BB_FRAME_STATS) have C++ handlers doing what their x86 stubs do; the recompiled code
   calls them; handlers call game functions through `bbport_guest_call`. **165,766 recompiled,
   7 original** (4 unsupported instructions, setjmp/longjmp, 1 CFG error; none for bbport's
   changes); gameplay OK, calls back into original game code 1.4M -> 1,421 per run, 59 FPS
   against 62 unmodified at the same time (the previous library matched: the sites' cost,
   to look at; back to back it measured the same, 61-62). Then BBSC: its ~250 hooks are sites,
   its 70 switched byte patches variants (both forms lifted, the code bytes choose), its game
   calls go through bbport_guest_call: one library for default, BB_FRAME_STATS and BBSC with
   online play, gameplay OK in each. Open for AArch64: the frame rate's ~50 redirected constants
   (byte patches pointing at bbport's page: lifted as patched, need that page at the same
   place); the runtime's own calls into game code (thread entries, library callbacks such as
   np_web_api's); the 7 functions left original. Next milestone: the game with no original x86
   code at all on this PC (every entry through recompiled code), then an AArch64 runtime
   (imports bridged to AAPCS64, C++ exceptions), then bbport on Android.
   **Where it stopped (2026-11-05):** a gameplay crash after a BBSC session was kit restore and
   the menu pointer calling game functions directly from handlers (fixed: BbGameFn; handler game
   calls go through recomp_hybrid_call_native). Item 1 (no original game code at all): profiled
   (perf, BB_AOT_TARGETS=all) and fixed in the recompiler: 16-bit shld/shrd (AMD's results past
   count 16, measured), jump tables reached only through other tables (0x210e5c0), no-return by
   shape (a function ending in a call with no ret or outward jump: 389; marking the trailing
   call's target too was wrong twice: FDE ranges end early), the startup's 0x20 and the static
   constructors its tables point at (DT_INIT/DT_FINI tables: 16,069, 13,233 with a CFG extent)
   lifted. Library `recompiler/out/aot/sites6`: 178,934 functions recompiled; gameplay OK, 61
   FPS, no traps. In 2 min of gameplay the only calls into original code are libc's longjmp
   (entry replaced by the runtime: no game code runs) and one call per constructor still
   original at startup. **2026-11-07:** those 2,837 (no extent) and the 69 "changed in bbport"
   were both wrong extents of functions with no FDE: a tail jump to an import stub (`jmp
   [GOT]`, after the game's code) counted as a block, stretching the extent over tens of MB of
   code bbport does patch (the 69), and the path ran past `__stack_chk_fail`'s stub into the
   padding (the 2,837: the extent pass had no noreturn test). Fixed in `game.py`
   `_extra_functions`; all 16,070 now have extents, none overlapping another function. Library
   `sites7` (`BB_RECOMP_MODE=abi bash recompiler/harness/build_aot.sh EBOOT
   recompiler/out/aot/sites6/list.txt out/aot/live-plain.bin,out/aot/live-stats.bin,out/aot/live-bbsc.bin
   sites7`, 582 MiB): **181,840 recompiled, 3 original** (libc's setjmp/longjmp, 1 unsupported
   instruction at 0x5733cd0). Gameplay OK with BB_FRAME_STATS: in ~3 min the only call into
   original code is libc's longjmp (1,350, entry replaced); same FPS as unmodified on the same
   walk (62 outside, ~45 in the first house: the scene, measured both ways). Then 0x5733cd0
   (libc's unwinder capture, unw_getcontext: fxsave, mov from cs/ss, pop to memory) lifted, and
   two fixes to BBSC's variants: a replacement with its own branch (`jge +4` at 0x191a8c3) sent
   its fall-through to the original code (9,092 jumps back for good per run), and the 11
   variants inside an instruction (BBSC's immediates: CoopMaskSearch, ShowTreasureShare...)
   were silently not lifted at all (the recompiled code kept the original constants with BBSC
   on). Library `sites9`: **181,841 recompiled, 2 original** (libc's setjmp/longjmp, both taken
   over by the runtime: no original game code runs). Gameplay OK in the default configuration
   and with BB_ONLINE=1 BB_BBSC=1: the only call into original code is longjmp's entry, 0 jumps
   back, FPS as unmodified on the same walk. Then the runtime's own entries: every call of a
   game function by bbport goes through `runtime_guest_entry` / `GUEST_FN` (the recompiled
   function's stub: the game's entry, module inits, thread starts, atexit, once routines, key
   destructors, the malloc-counter wrappers' originals, the heap-grow hook's release, and
   bbport_guest_call outside recompiled code); libc's longjmp is in the dispatch table; the
   module inits and entry points (no FDE) are lifted (`sites10`: 181,843 recompiled).
   **`BB_AOT_STRICT=1`**: the game's code pages lose execute permission after install; a native
   call of a game function faults, is sent to its stub and counted by caller. With `sites10`,
   default and BB_ONLINE=1 BB_BBSC=1, title to gameplay: **no native call of the game's code;
   none of the original x86 code runs.** Not covered yet: the online module's np callouts
   (gpu/bbnet20 calls their handlers natively; none fired in solo play), BBSC's code
   permission changes during a co-op session (they could make pages executable again), C++
   exceptions (the unwinder's resume; none seen). Next: the AArch64 runtime (the stubs become
   bridges: runtime_guest_entry is the one place). Toward it, the GPU side (port plan §1c): the
   shader resource table walker is also a portable program (`gpu/shader/ir/passes/
   srt_program.cpp`, kept in the shader cache, ShaderMetaVersion 9), the only walker on
   non-x86 hosts; `BB_SRT_INTERP=1` runs it on x86 (same FPS, rendering OK), `=verify` runs
   both and compares (33.5M walks in gameplay, none differ; 58 differed once, the tables
   changing between the two walks). `common/signal_context.h` has its AArch64 side (pc, ESR
   WnR), and `common/rdtsc.h` (CNTVCT_EL0; the runtime's TSC frequency must then report
   CNTFRQ_EL0) and `common/spin_lock.h` (yield) theirs; the renderer's "SSE" was TSC reads and
   spin hints only: `common/cpu.h` (Tsc, CpuPause, TscTicks(seconds): fixed cycle counts became
   times). Left in gpu/ for AArch64: code that reads the game's x86 registers from a fault
   context (shim/bbport_write_log.cpp, buffers/buffer_maintain.cpp, hooks.cpp, free_check,
   fault_sites: §1b, the recompiler's state holds them there) and the Xbyak hook trampolines.
   Then those too: fault code reads pc / sp / fp through `common/signal_context.h`; on AArch64
   InstallGuestCodeHook and hooks.cpp's Patch only register the guest sites (no trampoline, no
   int3: the recompiled game runs the handlers), the free check's label trap and the write
   log's register dump are x86-only. **The GPU library compiles for aarch64-linux-gnu: 291 of
   296 sources** (the other 5: this PC's OpenSSL headers are configured for x86; a target
   OpenSSL fixes them). Check: each source's own ninja command through `zig c++ -target
   aarch64-linux-gnu -fexperimental-library -c` (std::jthread is experimental in zig 0.13's
   libc++). Not linked or run on AArch64 yet. All compile for aarch64-linux-gnu (zig c++ -c; zig 0.13 fails with
   -fsyntax-only: "FileNotFound"). Diagnostics: BB_AOT_SKIP=addr,...,
   BB_AOT_TARGETS=all. The approach: on x86-64 the original and the recompiled code can run in one process. Patch
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
