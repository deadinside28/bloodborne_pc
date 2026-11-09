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
    every direct call resolved; compiled for AArch64: 243 MiB of code (6.1x the original; ~3.6x with register pruning, 2026-11-07).
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
   libc++). Not linked or run on AArch64 yet.
   Then the host-call bridge: `BB_IMPORT_DUMP` lists the 532 function imports the runtime
   provides; `scripts/import_signatures.py` reads their C signatures from the debug info (symbol
   at the address, then the DIE by name: GCC folds identical stubs, anonymous-namespace C++
   functions have only DW_AT_name): **all integers/pointers, none variadic, none with floats, at
   most 10 arguments** (job_run). So `BB_AOT_HOST_CALLS=portable` calls each one as a C function
   of ten integers (rdi..r9, then the four stack words past the return address), on the host
   stack, as an AArch64 runtime will; host_sp is lowered across the call (a callback into the
   game, e.g. a once routine, entered at the stack top over the live frames: RIP 0 at start).
   Gameplay with it and BB_AOT_STRICT, sites10: ~1.05G host calls, no fault, same FPS.
   And the other direction: `runtime_guest_call` / `GUEST_CALL` (src/runtime.h) for every call
   of the game by the runtime (entry, module inits, threads, atexit, once, key destructors, the
   malloc counters, bbport_guest_call outside recompiled code); `BB_AOT_ENTRY=portable` enters the
   recompiled function from C (`recomp_hybrid_call_recompiled`: state filled, guest stack below
   the innermost entry's or the C frame, frames on the thread's host stack through
   `recomp_call_on_stack`, x86-64 and AArch64 asm), no stub. fs_base and gs_base both the guest
   thread pointer (code lifted from bbport's patched bytes reads gs:; only fs_base: a crash
   loading into gameplay). Both portable and BB_AOT_STRICT: title to gameplay, ~2 min, ~760M
   host calls, no fault. Desktop driving: drive the game only when the active window is the game
   (bbkeys' KWin check gets no answer here; keys once went into the terminal).
   Then `src/` for AArch64: **every runtime file compiles for aarch64-linux-gnu with -Werror**
   (ABI is empty there; runtime_cycles: CNTVCT_EL0, the TSC frequency CNTFRQ_EL0; the guest
   thread pointer kept per thread, runtime_guest_thread_pointer, instead of arch_prctl; fault and
   watchdog reports through src/host_context.h; the unresolved-import trampolines in AArch64
   code; enter_on_stack in AArch64 asm). Left: recompiler/harness/recomp_hybrid.c is x86-only
   (it runs original code); AArch64 needs its recompiled-only part (thread state, entries from
   C, setjmp/longjmp for recompiled code) and runtime_aot.c's AOT mode without the stubs.
   Then that part: recomp_hybrid.c builds for AArch64 too (calls into original code and the entry
   patches stay x86's; __recomp_setjmp / recomp_hybrid_longjmp_resume in AArch64 asm, x19-x28,
   x29, d8-d15 in HybridJmp.saved); an entry made from C code already on the host stack starts
   below it. harness/portable_test.c (setjmp, longjmp from the same and from a nested entry made
   from C): passes on x86-64 and on AArch64 under QEMU, in run.sh. Then runtime_aot.c's AOT mode
   for AArch64: no stubs, patches or strict redirect; host calls and entries always portable; a
   guest target without recompiled code stops the game (aot_miss); rdtsc is runtime_cycles;
   cpuid answers a fixed Jaguar table (AuthenticAMD, family 16h, SSE-SSE4.2, AVX, F16C, POPCNT,
   MOVBE, BMI1, LZCNT); the thread pointer is runtime_guest_thread_pointer; x87 through the
   emulator. Compiles for aarch64 with -Werror (the whole runtime, x87 emulator included). Then
   the game library for AArch64: `BB_AOT_TARGET=aarch64 BB_RECOMP_MODE=abi BB_RECOMP_JOBS=8 bash
   recompiler/harness/build_aot.sh EBOOT recompiler/out/aot/sites6/list.txt <3 dumps> sites10`
   -> `recompiler/out/aot/sites10-aarch64/libbbaot.so`: 181,843 functions, 352 MiB, ~20 min; it
   imports only the AArch64 runtime's entry points (__recomp_dispatch, _jump, _guest_site, _trap,
   _setjmp, _x87, _x87_insn, _cpuid, _rdtsc, __recomp_slide). Next: a first run. No AArch64
   machine here: an AArch64 bb-probe in CPU-only mode (the GPU library and SDL stubbed) under
   qemu-aarch64 with an AArch64 glibc sysroot; it would run the game's startup up to its first
   graphics call.
   **Done (2026-11-07): the recompiled game runs on AArch64** (under qemu-aarch64 on this PC):
   `tools/a64_cpu_only.sh SYSROOT` builds bb-probe for aarch64-linux-gnu.2.38 with
   tools/a64_cpu_only_stubs.c (the GPU library, SDL3, Vulkan as no-ops) and runs it with
   --cpu-only, BB_AOT=sites10-aarch64 and BB_AOT_ACCEPT_CHANGED=1 (29 functions differ: no GPU
   library to patch the code). SYSROOT: Fedora 44's glibc and libgcc aarch64 RPMs (`dnf download
   --forcearch=aarch64 --arch=aarch64 glibc libgcc`, rpm -K OK, `rpm2archive - < x.rpm | tar -xzf -`,
   plus lib/ld-linux-aarch64.so.1 -> ../usr/lib/ld-linux-aarch64.so.1). Both module inits, the
   entry, libc, 16 guest threads (FMOD's mixer/output/stream, GXRenderThread, work pools), 283
   atexit registrations, 3,623 static guards, 1,683 mutexes / 10,986 locks, 4.3 GB of direct
   memory, files, 60 audio buffers out; it stops at the first graphics call
   (sceGnmGetTheTessellationFactorRingBufferBaseAddress: unresolved without the GPU library), as
   CPU-only must. Next: the GPU library for AArch64 (it compiles; needs AArch64 Vulkan, SDL3,
   boost... to link) and a machine with a GPU to run it on.
   **And on the phone (2026-11-07):** `tools/android_cpu_only.sh` builds bb-probe with NDK r30
   (`sdkmanager "ndk;30.0.16248370"`, aarch64-linux-android35-clang, -Werror), pushes it, the
   image and the library to /data/local/tmp/bbport (adb over Wi-Fi: ~450 MB in 8 s) and runs it:
   the Galaxy S25 Ultra (SM8750, Android 17) runs the same startup as QEMU, to the same first
   graphics call. Android needed: USER_MAX below 512 GiB on AArch64 (0x7000000000), no glibc
   mallopt tuning, no asm/prctl.h, strtok_r. Wireless adb: pair with the SDK's adb
   (~/.local/opt/android-sdk/platform-tools/adb; the pairing port from `adb mdns services`,
   _adb-tls-pairing) right after the code shows: stale codes fail with "protocol fault".
   Then the GPU library for Android: `tools/android_deps.sh` (SDL 3.4.18, fmt 11.2.0, Zycore +
   Zydis 4.1.1, xxHash 0.8.3 built with the NDK into out/android/deps; Boost, robin-map, VMA and
   the 1.4.357 Vulkan headers copied) and `tools/android_gpu.sh` -> out/android/gpu/libbbgpu.so.
   Changes: BB_FFMPEG (off on Android: no movies, platform/avplayer's Source never opens), no X11
   on Android, the Android Vulkan surface (WindowSystemType::Android, VK_KHR_android_surface,
   SDL's ANativeWindow), -z undefs (the runtime's symbols come from the executable), <algorithm>
   in read_sample.cpp, fmt with -include cstdlib. To note: the movie player calls the game's
   allocator callbacks natively (GuestBuffer): through runtime_guest_call before FFmpeg returns
   on AArch64. Then the APK: `tools/android_apk.sh` -> out/android/bbport.apk (android/: SDL's
   activity, libmain.so = bb-probe with src/android_main.c; no Gradle). Game data: `adb push` to
   /sdcard/Android/data/org.bbport/files/ (boot-linked.bin, content.bin, patches.bin, app0/ = the
   dump, 29 GB in ~10 min over Wi-Fi). No static TLS on Android (dlopened libraries: the
   initial-exec models are PC-only). **On the phone the app starts**: the Adreno 830 (Vulkan
   1.3.284, driver 512.800.64), a 3120x1440 swapchain, staging buffers, then STOP: the GPU cannot
   use the game's memory in place (no VK_EXT_external_memory_host, no dma-buf export of system
   memory: memory_model.cpp). That is the next problem: Phase 4's memory model on Adreno (to look
   at: AHardwareBuffer-backed guest memory imported with
   VK_ANDROID_external_memory_android_hardware_buffer and mapped at the guest's addresses through
   its dma-buf, or a copying model).
   `tools/android_memprobe.c` (NDK; push to /data/local/tmp and run over adb): on the S25 Ultra a
   hardware buffer of 64-256 MiB is allocated, gives its dma-buf fd (sendHandleToUnixSocket),
   maps at 0x1000000000 (MAP_FIXED_NOREPLACE), imports into Vulkan and is coherent both ways
   (GPU fill seen by the CPU, CPU writes seen by the GPU); 512 MiB and up are refused (-129).
   Adreno: AHB import, VK_KHR_external_memory_fd, buffer device address, descriptor buffer; no
   external_memory_host, no dma_buf handle type, no memory_budget. The runtime's CHUNK is 256 MiB
   already, but the layer memory module wants one chunk per direct allocation (up to 2.8 GB) and
   only lazy mode splits (with overlapping imports, impossible with separate AHBs). Next check:
   `memprobe opaque 512 1024 2048` (Vulkan memory exported as an opaque fd: mappable at the
   game's address and at offsets? then the PC model works unchanged). CAUTION: an uncapped
   allocation loop restarted the phone once; every mode is capped now (memory checked before
   each step, 2 GiB margin). Opaque fds: vkGetMemoryFdKHR fails (-1) for exportable memory.
   **The hardware-buffer memory model (2026-11-07):** gpu/shim/bbport_guest_memory_android.cpp:
   Mode::AndroidHardwareBuffer (AhbWorks at start: a 2 MiB buffer imported, its dma-buf mapped
   whole and at an offset), AllocAhbChunk for the runtime's 256 MiB grid (whole_chunks 0; a
   binding across chunks takes a copy), never while less than size + 2 GiB (BB_AHB_RESERVE_MB)
   would stay available (those chunks stay in the memfd). The APK's libbbaot.so names libmain.so
   (patchelf --add-needed: an app's namespace searches only needed libraries; RTLD_NOLOAD |
   RTLD_GLOBAL did not help). **On the phone the whole game runs**: 181,843 functions, 2.8 GB of
   direct memory in hardware buffers (the rest kept by the reserve), ~2G calls into bbport, 138k
   guest site calls, 18 command buffers submitted and decoded, then the guest submitted nothing
   for 5 s (GPU stall report with a thread dump: next to read). Runs on the phone: watch
   MemAvailable from the PC every 2 s and force-stop the app below 1.5 GB (it fell to 0.7 GB once
   near the end of a 2-minute run).
   **The Android emulator (2026-11-07):** SDK emulator 37.2.13, AVD `bbport` (Android 15
   google_apis x86_64, 8 GB, 48 GB data, -gpu host -feature Vulkan, headless): arm64 apps run
   through libndk_translation; Vulkan is gfxstream over the PC's GPU. Its allocator (ranchu)
   refuses BLOB hardware buffers with any GPU usage (GPU_DATA_BUFFER: UNSUPPORTED) and its Vulkan
   imports no CPU-only BLOB (properties fail); a CPU-only buffer's fd is
   /dev/goldfish_address_space, mappable only at the block's offset (memprobe finds it from the
   lock's mapping). So no in-place memory there: when hardware buffers do not work the Android
   mode now copies (SetAhbCopyOnly: every chunk stays in the memfd; BB_GUEST_MEMORY=inplace
   refuses). But its Vulkan (gfxstream, 1.3.0, 44 device extensions) lacks VK_KHR_push_descriptor
   and VK_EXT_robustness2 (required: instance.cpp), dynamic state 3, maintenance5: the renderer
   does not start there (and the emulator crashed in the device creation that followed). Use it
   for CPU-side checks only; the GPU path needs the phone.
   **The phone's configuration on the PC (2026-11-07):** sites10 strict with BB_AOT_HOST_CALLS=
   portable, BB_AOT_ENTRY=portable, BB_SRT_INTERP=1: 7 minutes, no stall; with
   BB_GUEST_MEMORY=copy (new: no chunk at all, every binding copied) and 60 Hz vblank too: 5
   minutes at 60 FPS, no stall. So the phone's 18-buffer stall is not in the portable paths or the
   copy path. Found instead: the presenter waited for a swapchain image forever, and an Android
   app in the background has no surface (SDL nulls ANDROID_WINDOW_POINTER) and gets a new one on
   return, which the swapchain never took: both stop the game's flips, all threads in futex
   waits (the phone's dump). Now: acquire times out after 1 s and skips the frame; a lost surface
   skips frames; the window thread (SDL_ANDROID_BLOCK_ON_PAUSE=0) tracks the surface, the
   presenter skips frames while there is none and makes the swapchain's surface again for a new
   one (Swapchain::RecreateSurface). Untested on the phone yet: run it in the foreground, then
   background and back. **The 18-buffer stall, found (2026-11-07):** not the background (it
   stalls in front too) and not a deadlock: the game's main loop paced empty frames (the frame
   pacer at guest 0x2034770 busy-waits ~2 ms a frame; BB_AOT_TARGETS=1 now samples calls into
   bbport with the guest return address). The game could not read its files: adb push makes
   directories `drwxrws--- shell ext_data_rw`, which the app cannot enter (files pushed straight
   into files/ are readable, so boot-linked.bin loaded): param.sfo unread (serial UNKNOWN, no
   game profile), app0 unreadable, loading never ends. After a push: `adb shell chmod -R a+rX
   .../files/app0; chmod -R a+rwX .../files/user`. Then the layer's copy path (bindings not whole
   in one chunk go to the sparse arena) stopped: Adreno's buffers reach 4095 MiB and arenas were
   4 GiB pages; arena pages now follow maxBufferSize (512 MiB on Adreno, 4 GiB elsewhere). The
   memory STOP prints its call stack and leaves with _Exit (exit's destructors under live GPU
   threads aborted on a destroyed mutex). The app takes extra KEY=VALUE settings from the launch
   (`am start ... --es env "KEY=VALUE ..."`). Next: memory: the game took ~2 GB in its first 2 s
   and the watchdog (1.5 GB available) stopped it; MemAvailable leaves out cached apps that the
   low-memory killer would close. **With the phone's other apps closed (5.3 GB available): the
   game runs on the phone, 60 frames a second for 3 minutes** (vblank 60 Hz; the title stage as
   on the PC), no stall, 2.3 GB of direct memory in hardware buffers and the rest copied (300-500
   bindings per 2 s across chunks through the 512 MiB arenas), MemAvailable steady at 2.0-2.7 GB.
   **Black screen (music plays):** the frame bbport presents was all black
   (BB_FINAL_DUMP_TRIGGER dump, read back with `adb exec-out run-as org.bbport cat`). Vulkan
   validation on the phone (`BB_APK_VALIDATION=<Khronos android-binaries
   libVkLayer_khronos_validation.so> tools/android_apk.sh`: layer packed, app debuggable; then the
   gpu_debug settings, see the script) found the chunk memory bound twice: Adreno requires a
   dedicated allocation for buffers over hardware buffers (01444) and the cache bound a second
   buffer over it (01508). Now the cache uses the chunk's own buffer (ShareChunkBuffers,
   Buffer(existing), adopted: not destroyed): no validation errors. Still black: the game renders
   (shader setup, job-system semaphores, 60 frames a second; no movie calls yet, ~6x fewer calls
   than the PC) but its frames are not the PC's UI-only title frames and come out black. Next:
   Adreno rendering: dump render targets, check the depth format fallback (no D16S8 attachment),
   the window orientation (a 1440x810 frame once: the window flips to portrait at start).
   A crash with BB_FRAME_STATS on the phone: the pad code's guest heap report called libc's
   malloc_stats natively (fixed: runtime_guest_call); AArch64 fault reports now name the caller
   (link register). Title stage on the phone: 18 draws a frame (PC: 22-24), 60 FPS, the presented
   frame still all zero; the device is lost now and then (no fault info on Adreno). No API
   errors under validation: next suspect is shader memory access through device addresses (the
   layer's page table over the hardware buffer chunks); GPU-assisted validation or a page-table
   check against the chunk buffers' addresses.
   Black screen, narrowed: not the hardware buffers (none at all: still black), not the copy path
   (PC with BB_GUEST_MEMORY=copy: a full frame), not the settings as such (PC at 1280x720: full
   frame). Found and fixed: the phone's BB_RENDER_RES (1280x720) did not match its patches.bin
   (made by run.sh at 2560x1440) or the library: the UI targets (2560x1440) were not recognised
   (BB_UI_TRACE=all: draws of frames without the UI before any had it); android_main now sets
   2560x1440, and every frame is a UI-only frame as on the PC. SDL turned the window to portrait
   (a resizable window overrides sensorLandscape): SDL_HINT_ORIENTATIONS landscape. Reserve
   BB_AHB_RESERVE_MB 4000 on the phone (lowest ~2.9-3.2 GB available, was ~1.5). The presented
   frame is still all zero: next, dump the UI target and the live host display buffer on the
   phone, and look for AArch64-only paths in the GPU library (CPU texture conversion, the SRT
   program) that the PC never takes. Checked since: the PC with BB_SRT_INTERP=1 dumps a full
   frame; the code hooks are in the library's sites (BB_AOT_DUMP's .sites lists them: 14 of 56);
   the upscaler dump does not run in UI-only frames; BB_PRESENT_DUMP_TRIGGER and GPU-assisted
   validation (debug.vulkan.khronos_validation.validate_gpu_based) both ended in a lost device
   (now within 5-25 s in about every second run) before reporting. Ideas: --vulkan-only on the
   phone (does the presenter show anything), the draws one by one (BB_SKIP_PS), Adreno-specific
   shader translation differences. More ruled out on the PC: 512 MiB arena pages
   (BB_ARENA_PAGE_MB=512), the phone's missing extensions (BB_IMITATE_ADRENO=1: dynamic state 3,
   maintenance8, atomic float 2, barycentrics, compute derivatives, feedback loop, budget...), BC
   textures (Adreno has them all). All three together with BB_GUEST_MEMORY=copy: a full frame. On
   the phone the live host display buffer (BB_PRESENT_DUMP_TRIGGER) is opaque black (alpha set,
   colour 0). Left: Adreno's limits and features inside its extensions (storage images without
   format, alignments, push descriptor limits) and its shader compiler; a frame capture
   (RenderDoc for Android) would show which pass goes black. Limits compared (phone / PC):
   subgroup stages compute+fragment only / all (the shader compiler now logs subgroup use
   outside them; none at the title), no clustered subgroup ops, no shaderFloat64, storage/uniform
   offset alignment 64/256 (16/64), 7 descriptor sets (32), subgroup 64 (32). Validation was clean.
   **Found with RenderDoc (2026-11-08):** captured on the phone (BB_RENDERDOC_TRIGGER, RenderDoc's
   layer packed into the APK: BB_APK_LAYERS; gpu_debug_layer_app was not searched) and replayed
   on the phone through RenderDoc's remote server, driven by a small C++ tool against
   librenderdoc's replay API (tools/renderdoc: the tools and how to use them) (qrenderdoc needs an analytics answer first; the PC cannot replay the
   capture: APIHardwareUnsupported). The display pass (game vs 89cda9e4) reads a good
   1920x1080 image and the 1025x1x3 display LUT, but writes no pixel: its output keeps the clear
   (alpha 1 everywhere). It is a RECT LIST drawn as a 3-control-point patch through bbport's
   auxiliary tessellation shaders (emit_spirv_quad_rect.cpp); push constants (960/540 scale and
   offset), viewport (16384, the limit), scissor and raster state are right, the fragment
   shader is right (no fragment reached it). So Adreno's tessellation of these patches produces
   nothing. Tried, no change: the control shader reading a plain Position built-in (the vertex
   shaders write one) and no clip/cull distances in the aux blocks (both kept: valid and cleaner;
   ShaderMetaVersion 11). Next: rect lists without tessellation on Adreno (a geometry shader, or
   the fourth vertex computed by a second vertex invocation), or a minimal tessellation test on
   the phone. Captures under RenderDoc now fail at startup (ConstantRing: out of device memory).
   **BLACK SCREEN FIXED (2026-11-08): the title renders on the phone.** Cause: Adreno's driver
   (512.800) zeroes a fragment shader's colour when it declares `DenormFlushToZero 32` (it claims
   shaderDenormFlushToZeroFloat32). Found by replaying the captured display pass on the phone
   with its shader swapped (rdpatch: RenderDoc's BuildTargetShader + ReplaceResource, variants
   made with the NDK's spirv-dis/spirv-as): without that one execution mode a white input came out
   white, with it black. pipeline_cache.cpp: no denormal flush on Qualcomm (BB_DENORM_FLUSH=0/1
   overrides); ShaderMetaVersion 13. Earlier guesses on the way (rect lists by tessellation, the
   table's LOD, separate samplers, compute-written images) were ruled out with RenderDoc's pixel
   history and small probes (scratchpad lut1d): tessellation works on Adreno; rect lists by a
   geometry shader stay as an option (BB_RECT_LISTS=geometry, same frame on both GPUs). Real
   fixes kept from that search: a required subgroup size only on stages the device lists
   (requiredSubgroupSizeStages: Adreno compute only); the aux control shader reads a plain
   Position, the aux blocks carry no clip/cull distances. Still open: the device is lost in
   about one run of two within ~20 s (no fault info). The phone over USB (adb -s R5CY11JMTEY)
   when wireless debugging fails; the watchdog's adb calls are time-limited now.
   The intermittent lost device is not the build without FFmpeg: the PC with that GPU library
   (cmake -DBB_FFMPEG=OFF into out/gpu-noff, LD_LIBRARY_PATH over the RUNPATH) renders the
   title for 90 s; the game makes no movie calls at the title at all (the opening plays after
   New Game). **Movies on Android:** tools/android_ffmpeg.sh builds FFmpeg 8.0 (the PC's API,
   libavcodec 62) static and minimal (mov demuxer, H.264 and AAC decoders and parsers, swscale,
   swresample: the game's four MP4s are H.264 + AAC); android_gpu.sh turns BB_FFMPEG on when it
   finds it; linked in one --start-group with -Bsymbolic (FFmpeg's AArch64 assembly addresses
   its tables directly). Untried on the phone.
   **First-run setup in the app:** SetupActivity is the launcher; with the game's files missing
   it asks for a folder (Android's picker) and copies it into the app's files folder with
   progress, a free-space check (2 GB stay free) and resume (same-size files skipped; .part
   while copying), then starts BbportActivity. The folder is tools/android_prepare.sh's
   (out/android/bbport-data: run.sh's three .bin files, app0/ for the dump). Files the app copies
   are its own: no chmod. Tried on the emulator (fake folder: picker driven with uiautomator,
   checksums equal).
   **Touch controls (BB_TOUCH_CONTROLS, on by default on Android):** gpu/shim/bbport_touch.cpp
   takes the window's finger events (and taps' mouse clicks, so they do not take the mouse):
   left of 45% of the width a floating stick, right of it a floating camera stick (the origin
   follows a finger that goes past the radius), DS4 buttons drawn by the overlay (face, L1/L2,
   R1/R2, R3 lock-on, L3, d-pad, Touch, Options, and Menu for the port's menu). runtime_pad's
   sample_host reads them (bbgpu_touch_pad) over the pad state; they hide and read nothing while
   a gamepad is connected or the menu is open; prompts are the PS4's while they are shown. On a
   PC with BB_TOUCH_CONTROLS=1 the mouse is the finger (SDL_HINT_MOUSE_TOUCH_EVENTS). Drawn on
   the PC (title); the logic checked with a standalone test fed SDL finger events. Untried on
   the phone.
   (PC builds outside nix-shell: CPLUS_INCLUDE_PATH=~/.local/include, as the
   build folder was configured with it.) All compile for aarch64-linux-gnu (zig c++ -c; zig 0.13 fails with
   -fsyntax-only: "FileNotFound"). Diagnostics: BB_AOT_SKIP=addr,...,
   BB_AOT_TARGETS=all. The approach: on x86-64 the original and the recompiled code can run in one process. Patch
   function entries (the way bbport's guest-code hooks do) to jump into recompiled functions for a
   chosen set, grow the set, and bisect when the game misbehaves; then run with no original code.
   Needs: the linked PRX modules lifted too (`scripts/link_modules.py` image), the game's 661 GOT
   imports bound to bbport's runtime, bbport's own code hooks as a recompiler input table, and C++
   exceptions (38 functions with handlers: the unwinder jumps into the middle of functions).
3. Multi-threaded tests of `lock` atomicity and of `--tso` ordering, on the phone.
4. Code size (6.1x): flags and register write-backs at calls and exits are the main cost.
   **Registers a function never touches no longer cross its calls (2026-11-07,
   `BB_RECOMP_PRUNE`, on by default):** spill/reload leave markers that `expand_markers` fills
   with the registers the body uses; the others' state slots already hold their values. Measured
   on a 2,260-function AArch64 shard: 4.64 -> 2.70 MB (-42%), host stack spills 109k -> 11k
   (the i128 vector registers lived in GPR pairs). Whole game, x86-64 (`sites11`): 582 -> 220 MiB;
   title 3 min strict, no native call; gameplay (drive.sh: the first house, four walks) strict
   and portable, renders right, same FPS as sites10 within noise (median 46.7 vs 45.9). AArch64
   (`sites11-aarch64`): 352 -> 210 MiB (-40%). Corpus: 144 configurations pass. Next candidates: the ABI's
   callee-saved registers across direct calls (not rbp: frame chains; setjmp needs them all),
   and the state stores before calls of scratch registers no callee reads. Tried (2026-11-07):
   rbx and r12-r15 kept in the caller across calls: -3.4% more on the same shard, but it breaks
   `corpus/nonabi.S` (hand-written helpers returning in callee-saved registers, which --abi
   supports by design) and would leave stale saves for the C++ unwinder: not done.
   **`--tso`'s inline check (2026-11-07):** with the fault mode (`BB_RECOMP_RCPC=fault`: bare
   ldapr/stlr, crossing accesses left to a SIGBUS handler) the shard is 2.70 -> 1.68 MB (-38%).
   But measured (`BB_RECOMP_COUNT_CROSS=1` x86-64 build, `__recomp_cross` counts by instruction,
   "AOT cross:" lines): in gameplay ~720k shared accesses a second cross 16 bytes, from ~2,800
   instructions; the hottest are unaligned struct fields (vmovlps [rbx+0Ch], mov [rax+18h], mov
   qword [rbx+2BCh], 0) and one in a system module. At ~2-5 us a fault, faulting on all of them
   costs more than a core: not as is. Next: the check only at instructions a profile saw cross
   (a list for the lifter), the fault handler (recomp_loader.c's, into the runtime) for the
   rest, with fault counts by site to grow the list. **Done (2026-11-08), untested on the phone:**
   BB_AOT_CROSS_OUT=path writes every crossing instruction with its count (merged across runs;
   the profile in recompiler/out/aot/cross-sites.txt: 2,792 from the title and the drive.sh
   route); BB_RECOMP_RCPC=profile BB_RECOMP_CROSS_SITES=<that file> keeps the inline check only
   there (lift.py checks_crossing); runtime_aot_alignment_fault (AArch64, from probe.c's SIGBUS)
   does the other crossing accesses byte-wise with their barrier and counts their function (bit
   63 in the same file: the guest map is per function; the next lift checks all of its
   accesses). `sites12-aarch64`: 210 -> 156 MiB. Next on the phone: the "AOT cross" lines show
   how often the fault path runs; then tools/android_apk.sh with sites12.
   **Where sites12-aarch64 goes (2026-11-08):** .text 129 MB, 32.2M instructions for 10.7M guest
   ones (3.0x). Loads and stores through x19 (the guest state) are 9.8M, 30%: about 15 per call
   site over 640k calls, so the guest registers' sync at calls is still the main cost. LLVM's
   outliner does not reach it: -moutline on libz is -0.6% (-Os no better). The next step has to
   be in the lifter (fewer registers synced per call), not compiler flags.
   **Character creator on the phone (2026-11-08):** the model shows. Three causes found: (1) only
   some 256 MiB chunks are hardware buffers; a paged binding sends writes to pages without one to
   a scratch buffer, so the copy shader's results (bone matrices) were lost: BB_COPY_ON_CPU, on
   by default on Android and with BB_GUEST_MEMORY=copy (which reproduces this on a PC). (2) Depth
   writes were lost until dynamic state is set again at every draw: BB_DYNAMIC_STATE_ALWAYS=1,
   Android's default for now; which state goes stale is not known. (3) Object motion off and
   BB_BREADCRUMB_SPLIT=1 are still needed: the GPU is lost at the second 3D frame otherwise.
   Open: the cape is missing in every frame (only its collar shows; the G-buffer is steady over
   90 samples, background pipelines not involved); the load after creating a character.
   The cape: on the PC, recompiled x86-64 code (sites11) with BB_GUEST_MEMORY=copy shows it, so
   the lifter's logic is not the cause; the phone's AArch64 code or GPU is. The copy filter
   (BB_COPY_SKIP_SAME=0) changes nothing. Sampled vertex streams differ between phone and PC in
   many draws, in both directions: not yet tied to the cape's draw.
   tools: scripted runs (New Game to the creator): out/android/drive.sh, a pad file.
   PC crash skipping the second cinematic: garbage vtable pointer in GXRenderThread at
   +0x1f591b0 (`call *0x18(%rax)`, object at rbx+0x38); intermittent: 1 hit in 18 scripted runs.
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
