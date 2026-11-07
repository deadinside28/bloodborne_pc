# Android / ARM64 port plan

Status: plan only, nothing implemented. Written 2026-11-03 against `master` (shallow clone).
Scope: a native AArch64 Android build of bbport: the game's x86-64 code is recompiled ahead of
time to AArch64; the runtime and GPU core are built natively for Android. No x86 emulator
ships in the final product (one is used as a fallback during bring-up, see Phase 3).

## 0. Starting point

What exists:

- `bbport` runs the game's original x86-64 code in place (`src/probe.c` maps the linked image
  and jumps to its entry). `src/runtime_*.c` (~6,100 lines) replaces the PS4 OS; `gpu/`
  translates PM4 + GCN shaders to Vulkan/SPIR-V.
- `docs/CPU_RECOMPILATION_ANALYSIS.ru.md` describes a recompiler pipeline
  (SELF → ELF → CFG → LLVM IR → x86-64/AArch64 objects) and a prototype: 140 leaf integer
  functions (no memory access, no calls, no branches), checked in QEMU only.
  **The `recompiler/` directory it describes is not in the repository** (absent on `master`
  and `testing`, never committed under that path). We start without it.
  **Update (2026-11-03):** a new recompiler was started on branch `recompiler`, see
  [recompiler/README.md](../recompiler/README.md) for its status. It has not been run on the
  game yet.
- `gpu/common/arch.h` already defines `ARCH_ARM64`, so the GPU core has some prior
  ARM-awareness (inherited from shadPS4).

Facts about the binary (from the analysis doc, eboot 1.09):

| | |
|---|---:|
| Unwind ranges (≈ functions) | 162,959 |
| Code bytes covered by them | 41.7 MB |
| Relocations (mostly relative pointers into code) | 235,000 |
| Instructions touching XMM / YMM | 844,157 / 8,427 |
| AVX-512 | none |
| Linked PS4 modules also needing translation | `libc.prx` (752 KB RX), `libSceFios2.prx` (310 KB RX) |

A constraint that shapes every phase: **the recompiled code is derived from the user's dump
and cannot be distributed.** The pipeline must run on the user's own data: on a PC, which then
builds an APK or data pack to sideload, or on the device itself.

## 1. Inventory of x86-specific code outside the game

Counted by grepping `src/`, `gpu/`, `tests/` (third_party and vendored shadPS4 excluded).

### 1a. Loader and runtime (`src/`)

| Where | What | ARM64 replacement |
|---|---|---|
| `probe.c:15` | `#error` unless `__x86_64__` | split into an x86 loader and an AOT loader |
| `probe.c:104-110` | `cpuid` / `xgetbv` feature checks | drop (AOT code has fixed requirements) |
| `probe.c:186` | `enter_on_stack` asm, import trampolines | AAPCS64 entry + SysV→AAPCS64 call bridge |
| `probe.c:199`, `runtime_thread.c:5,70` | the game's `fs:[0]` is relinked to `gs:[0]`; `arch_prctl(ARCH_SET_GS)` per guest thread | the recompiler lowers FS accesses to a load from a reserved register (e.g. x28 holds the guest TCB); no kernel call |
| `runtime_kernel.c` (7 hits) | ucontext / register access | per-arch accessors |
| `scripts/link_libc.py`, `link_modules.py` | link PRX x86 code into one image, shared import table | the same link map feeds the recompiler; imports become direct calls into the C runtime |

### 1b. Guest code hooks: the hardest coupling

The port changes the game's behaviour by **patching its x86 bytes at runtime**:

- `gpu/games/bloodborne/hooks.cpp` (15 hits): `int3` traps and `jmp rel32` detours at fixed
  guest offsets (heap allocator, loader copies, Gx node, release check), handlers that rewrite
  `REG_RIP/RAX/RSP/RBP/R12/R13` in the signal context.
- `gpu/shim/bbport_guest_code_hooks.cpp` (14 hits): Xbyak-generated trampolines saving YMM
  registers, used for the frame-time hooks (unlocked frame rate) and observers.
- `gpu/shim/bbport_free_check.cpp` (12 hits): page-protect + x86 Trap Flag single-stepping.
- `gpu/memory/fault_sites.cpp`, `page_watch.cpp`, `layer/layer_traps.cpp`: key statistics by
  the faulting `REG_RIP` and walk the guest stack through `RBP`/`RSP`.

None of these survive recompilation as written: the bytes they patch are not executed.
Replacement: **hooks become a recompiler input.** A table (guest address → host function,
mode: replace / before / after) is applied while lifting, so the hook is a direct call in the
generated code: no traps, no trampolines, and faster than today. The `int3` handlers that
edit registers become C functions that receive and return the guest register state
explicitly. Debug-only machinery (Trap Flag single-step in `free_check`) is dropped or
reimplemented with a recompiler instrumentation mode.

Fault-address logging needs a map from host PC to guest address (emitted by the recompiler as
a sorted range table) so that `fault_sites` keeps reporting guest offsets.

### 1c. GPU core (`gpu/`)

| Where | What | ARM64 replacement |
|---|---|---|
| `shader/ir/passes/flatten_extended_userdata_pass.cpp` | Xbyak JIT that walks the shader resource tables; non-x86 branch is `UNREACHABLE_MSG` | write the walker as plain C++ (an interpreter of the same walk, no JIT needed) or emit AArch64 with a small assembler (oaknut, as shadPS4/Dynarmic use) **Done (2026-11-07):** the walk is also a program (`srt_program.cpp`), run on non-x86 hosts; checked against the x86 code in gameplay (`BB_SRT_INTERP=verify`: 33.5M walks, none differ) |
| `common/rdtsc.h` | `__rdtsc` + `lfence` | `cntvct_el0` + `isb` (and its frequency `cntfrq_el0`) **Done (2026-11-07).** |
| `common/spin_lock.h` | `_mm_pause` | `yield` / `wfe` **Done (2026-11-07).** |
| `common/signal_context.h` | `REG_RIP`, `REG_ERR & 2` (write fault) | `uc_mcontext.pc`; write fault from ESR in `__reserved` (`ESR_MAGIC`) on Linux/Android **Done (2026-11-07).** |
| `render/vk_draw_pipe.h`, `vk_bind_helper.h`, `raster_pipe.cpp` (27 hits) | SSE intrinsics for state hashing/copies | `sse2neon` short-term, then NEON or portable code **Done (2026-11-07):** these were TSC reads and spin hints, not SSE: `common/cpu.h` (Tsc, CpuPause, TscTicks); fixed cycle counts became times |
| `buffers/*`, `memory/read_sample.cpp`, `vulkan/timeline.*` | scattered intrinsics / `REG_RIP` | the same treatment |
| `dlss_bridge/`, FSR 4 / 4.1.1 | NVIDIA / RDNA-specific | not built for Android; FSR 1 and FSR 3.1 stay |

`gpu/platform/win32/*` is irrelevant to Android.

### 1d. Address space

`runtime_memory.c:54-55`: guest user memory is `[0x1000000000, 0xfc00000000)`, and
`low.c` notes that **PS4 code packs pointers into 40-bit fields**, so guest addresses must stay
below 2^40 (1 TiB) and the game's addresses must be real host addresses (identity mapping).

Many Android arm64 kernels use 39-bit virtual addresses (512 GiB user space, top
`0x8000000000`). On such a device `USER_MAX` is unreachable. Options, best first:

1. Measure the game's real peak address; if it stays under ~0x7f00000000, shrink `USER_MAX`
   and keep identity mapping (cheapest, try first).
2. Have the recompiler add a guest base to every memory access (a "membase" register). Costs
   about one `add` per access (often folded into the addressing mode), and frees the layout
   entirely, but the GPU side must apply the same offset to every guest address it receives.
3. Require 48-bit-VA devices (rules out many phones).

Verifying the target devices' `VA_BITS` is a Phase 0 task.

**Measured (2026-11-04, Galaxy S25 Ultra, Snapdragon 8 Elite, Android 17, kernel 6.6 GKI,
`recompiler/harness/device/va_probe.c`):** 39-bit user address space, 4 KiB pages. `0x0800000000`
and `0x1000000000` map at their own addresses, `0x8000000000` and above do not; the largest
contiguous reservation from `0x1000000000` is 402 GiB of the 944 GiB the port reserves today.
Option 1 therefore depends on the game's real peak guest address staying below roughly
`0x7000000000`, which the Phase 0 profile on PC has to measure. CPU features: LSE, LRCPC,
LRCPC2 (`ilrcpc`) and LSE2 (`uscat`: misaligned atomics within 16 bytes do not fault).

## 2. The recompiler: design decisions

| Decision | Choice | Why |
|---|---|---|
| Decoder | Zydis 4 (already in the doc's toolchain) | exact, complete AVX/BMI coverage |
| Semantics | Remill (amd64 → LLVM IR), wrapped with our own CFG/function model | writing full x86 semantics (flags, partial regs, SSE/AVX, x87 if any) from scratch is the single largest risk; Remill already has it |
| Unit of translation | function (FDE range), whole-image link map | FDEs give 163k ranges with no overlaps; relocations give most indirect targets |
| Register state | guest registers in a context struct, promoted to host registers by LLVM within a function | correct by construction; LLVM removes most loads/stores |
| Flags | computed lazily, only where read | x86 code sets flags on nearly every op; eager flags would double code size |
| Indirect calls/jumps | lookup in a guest-address → host-function table (perfect hash), generated from relocations + CFG; miss → fallback (below) | vtables and callbacks are pointers in data (`149,926` relative relocs point at FDE starts) |
| Fallback for unknown code | an interpreter or embedded x86 JIT (FEX-Emu core or Box64 dynarec, linked as a library) for addresses missing from the table, with logging so they are added to the next AOT run | static recompilation of a 90 MB game never reaches 100% coverage first time; a fallback makes it boot anyway |
| Memory ordering | x86 is TSO, ARM is not. Default: every guest load is `ldapr`/`ldar`, every store `stlr` (`ldapr` needs FEAT_LRCPC: every ARMv9 core, e.g. Snapdragon 8 Gen 1 and newer; verify in Phase 0); then relax provably thread-local accesses (stack, non-escaping) to plain `ldr/str` | plain stores break the game's lock-free code; full `dmb` fences are 2-5x slower |
| `lock`-prefixed ops | LSE atomics (`ldaddal`, `casal`, `swpal`) | ARMv8.1+, present on every target device |
| SSE/AVX | LLVM vector IR → NEON; 256-bit YMM split into two 128-bit ops | NEON is 128-bit; YMM use is small (8,427 insns) |
| FP | preserve MXCSR rounding modes and DAZ/FTZ via FPCR; x86 NaN propagation and `cvttss2si` overflow (0x80000000) need explicit sequences | games rely on these silently; mismatches show up as physics/animation bugs |
| Exceptions | the 38 FDEs with an LSDA: lower to explicit control flow, or keep a tiny guest unwinder for them | small count, handle by hand |
| Calling the runtime | imports bind to C functions through generated SysV→AAPCS64 shims | the runtime is plain C and can be called natively |
| Output | one AArch64 `.so` + address tables + host-PC→guest map | loaded by an AOT loader replacing `probe.c`'s jump |

Correctness harness (built in Phase 1, used in every later phase):

- **Differential testing on x86-64**: compile the same IR back to x86-64 and run it in place
  of the original functions in the existing bbport (`BB_AOT=1`), function by function. Any
  divergence is caught on the PC, with the real game, before ARM is involved at all. This is
  the most valuable tool in the whole plan: the existing x86 port becomes the oracle.
- Unit vectors per function class, as the prototype did (seeded inputs, full register file
  and memory side effects compared, not only RAX).
- AArch64 runs under QEMU user mode on the PC, then on a real device.

## 3. Phases

Estimates are for one experienced engineer (compilers + low-level x86/ARM), full-time.
They are wide on purpose: Phases 1–3 carry most of the uncertainty.

### Phase 0: groundwork (2–4 weeks)

- Contact upstream: ask for the unpublished `recompiler/` and coordinate (a fork of a fast-moving
  project is a maintenance burden). This could save weeks of Phase 1.
- Pick 2 target devices (suggested: one Snapdragon 8 Gen 2/3/Elite phone or handheld, e.g.
  AYN Odin 2 or Retroid Pocket 5: Adreno, Turnip available, 12–16 GB RAM).
- On each: check `VA_BITS`, page size, `FEAT_LRCPC`/LSE, Vulkan 1.3 and the memory extensions
  (§4).
- On PC: measure the game's peak guest address, RAM use, CPU time per thread, and the hottest
  functions (perf on the x86 build). This decides §1d and where optimisation effort goes later.
- Set up toolchain: Android NDK r27+, LLVM 21, Zydis, Remill, QEMU user, CMake/Ninja.

Exit: written device matrix + profile; go/no-go on the address-space option.

**Status (2026-11-04):** one device measured (§1d: Galaxy S25 Ultra, 39-bit address space, LSE,
LRCPC, LRCPC2, LSE2). Not done: the PC profile of the game's peak guest address and hot functions.

### Phase 1: recompiler core on x86-64 (3–5 months)

- CFG recovery over all 163k ranges: switch tables, tail calls, landing pads; resolve the
  analysis doc's 22 decode failures, 3 conflicts, 5,635 out-of-range exits.
- Lifting through Remill with our function model, lazy flags, guest context.
- Indirect-target table and fallback hook (fallback = jump back into the original x86 bytes,
  which on x86-64 simply works).
- Hooks-as-input (§1b): port `hooks.cpp` and the frame-time hooks to the table format.
- Run the AOT x86-64 build inside the existing bbport, growing coverage until the game reaches
  the title screen, a save, and gameplay with **zero fallback hits** on a fixed route.

Exit: AOT x86-64 build plays like the original on PC. This proves the translation without any
ARM, Android or driver variable.

**Status (2026-11-04):** the recompiler exists (`recompiler/`, branch `recompiler`). Python front
end (iced-x86 decoder, own CFG and jump-table recovery, ~600 instruction semantics) emitting LLVM
IR. Differentially tested against original code (144 corpus functions in 54 x86-64 and AArch64
configurations; the system's libz, 1,581 checks). On the game, from FDEs alone: 162,961 functions,
9.98M instructions, 3 unsupported sites (junk decodes), 24 decode failures (the analysis above found
22), every direct call resolved (63,641 through import GOT slots), 3,915 jump tables. The lifter is
not the schedule risk it was estimated to be (Remill was not needed); what remains of Phase 1 is
the integration into bbport: PRX modules, NID imports, hooks as input, `BB_AOT=1`. x86 memory
ordering measured on the phone: 1.08× with acquire/release (`ldapr`/`stlr`) and an inline
alignment check, against 3.5× with barriers, so risk 2/6 in §5 is much smaller than feared.

### Phase 2: retarget to AArch64, Linux (2–3 months)

- Memory ordering model, LSE atomics, NEON lowering, FPCR handling, YMM splitting.
- AOT loader for AArch64: TCB register, SysV↔AAPCS64 shims, host-PC→guest map.
- Port `src/` and `gpu/` to aarch64 Linux (§1a, §1c): userdata walker, rdtsc, spin lock,
  signal context, intrinsics via sse2neon.
- Bring-up on a **Linux ARM64 machine with a desktop-class GPU first** (e.g. an Ampere or
  Snapdragon X Elite box running Linux with Mesa/Turnip, or an Asahi Mac with Honeykrisp
  Vulkan). Same OS as today, so only the CPU changes.
- Fallback here is the embedded x86 JIT (FEX/Box64 as a library) for missing addresses.

Exit: title screen → gameplay on ARM64 Linux.

### Phase 3: Android runtime (1.5–3 months)

- Build with the NDK against bionic. `src/runtime_*` uses POSIX + `syscall`; check every
  Linux-only call (`memfd_create`, `SYS_arch_prctl` gone, `MAP_FIXED` ranges, futexes,
  `sched_setaffinity` for big/little cores).
- Window, input, audio through SDL3's Android backend (SDL3 already a dependency); a minimal
  Java/Kotlin `SDLActivity` with a settings screen replacing `launcher/` (Python/Tk is not an
  option on Android).
- Storage: Storage Access Framework picker for the dump; the pipeline output and saves live
  in app-private storage.
- Lifecycle: pause on background, surface loss/recreate (Vulkan swapchain), thermal throttling.
- Packaging: the APK ships the runtime + GPU core only; the recompiled game `.so` is produced
  from the user's dump (§0) and imported, or the APK is built locally by a PC tool.

Exit: game boots and plays on the target device.

### Phase 4: GPU on mobile drivers (1–3 months, overlaps Phase 3)

See §4. Also: tune the unified-memory path (`docs/unified_memory.md`; it already exists for the
Steam Deck), the shader cache pre-built for the device, a low default resolution with FSR 1 or
FSR 3.1 upscaling, and Adreno/Mali driver bugs (expect several).

### Phase 5: performance (open-ended, 2+ months)

The PS4 CPU is 8 Jaguar cores at 1.6 GHz; a current Snapdragon is much faster per core but the
recompiled code will start well below native. Targets, in expected order of payoff:
relaxing the TSO model where it is provably safe, flags elimination, inlining across hot
call edges, and profile-guided AOT using Phase 0's hot list.

### Total

Roughly **10–18 months** for one person to a playable, unoptimised build on one or two
devices, assuming Remill holds up and the address space option 1 or 2 works. Upstream's
unpublished recompiler, if shared, mainly shortens Phase 1.

## 4. Vulkan requirements on Android

The game's memory must be GPU-visible at the game's own addresses (`docs/unified_memory.md`):

| Way | Android drivers |
|---|---|
| dma-buf chunks mapped at guest addresses (`VK_EXT_external_memory_dma_buf`) | Turnip (Mesa, Adreno 6xx/7xx; 8xx support is newer), PanVK (Mali, early). Not exposed by stock Qualcomm/ARM drivers to apps. |
| host-pointer import (`VK_EXT_external_memory_host`) | Turnip; stock Adreno/Mali drivers: patchy, must be checked per device |
| `VK_EXT_map_memory_placed` (not implemented in bbport yet) | Mesa drivers are adding it; check Turnip's support on the target GPU in Phase 0. Would be the cleanest fit, worth implementing |
| AHardwareBuffer import | universal on Android, but its mapping cannot be placed at a chosen address; would need the membase model (§1d option 2) on the GPU side too |

Also required: Vulkan 1.3, `bufferDeviceAddress`, 64-bit integers in shaders. Practical
consequence: **the first target is Snapdragon devices with Turnip loaded per-app** (as
emulator front-ends such as Yuzu/Eden forks do through `libadrenotools`). Stock-driver and
Mali support are a later, separate effort.

RAM: the unified-memory path leaves at least 8 GiB to the system; Bloodborne itself expects
up to 5 GiB of PS4 memory. Realistic minimum: 12 GB device, 16 GB recommended.

## 5. Risks, highest first

1. **Recompiler completeness and correctness**: 163k functions, data-held code pointers,
   self-referential tables. Mitigated by the x86-64 differential harness and the runtime
   fallback; it remains the dominant schedule risk.
2. **Memory-ordering bugs**: rare races that only show on ARM, appear as hangs or corruption
   hours into play. Start strict (all acquire/release), relax only with proof.
3. **Address space** (§1d): 39-bit VA devices may force the membase model, which touches the
   GPU core as well.
4. **Mobile GPU drivers**: placed/imported memory may be missing on stock drivers; Turnip on
   the newest Adreno generations lags.
5. **Upstream churn**: the hooks and the GPU core change weekly (`docs/CHANGES_*`); every hook
   added upstream needs a table entry here. Keep the hook table as the single place they live.
6. **Performance**: unknown until Phase 2; a strict TSO model alone may cost 30–50%.
7. **Legal/distribution**: never ship derived game code; the user runs the pipeline.

## 6. First concrete steps

1. Open an upstream issue asking for `recompiler/` and whether ARM64 work is planned.
2. Profile the existing x86 build (Phase 0 list): peak guest address, hot functions, RAM.
3. Check `VA_BITS`, LRCPC and the Vulkan memory extensions on the candidate devices
   (`getconf PAGESIZE`, `/proc/cpuinfo` features, `vulkaninfo`).
4. ~~Prototype the userdata walker without Xbyak~~ (done 2026-11-07, §1c): needed for any non-x86 build, small, and
   upstreamable on its own.
5. Prototype hooks-as-input on x86-64 with one hook (the heap allocator site in
   `hooks.cpp`) before building the full lifter around it.
