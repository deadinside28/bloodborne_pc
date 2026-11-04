# recompiler: x86-64 → LLVM IR → x86-64 / AArch64

Static (ahead-of-time) recompiler for the game's x86-64 code: Phase 1 of
[docs/ANDROID_ARM64_PORT_PLAN.md](../docs/ANDROID_ARM64_PORT_PLAN.md). Not connected to bbport
yet, and not yet run on the game's eboot.

## Status

| | |
|---|---|
| Inputs | ELF64 x86-64 images; the PS4 executable (`eboot.bin`, plaintext SELF, through bbport's `scripts/prepare.py` parser, `recomp/ps4.py`); functions from symbols or from `.eh_frame` FDEs (the game's only source, found through `PT_GNU_EH_FRAME`) |
| Control flow | recursive descent per function; tail calls; calls that never return (13 of the game's imports, e.g. `__stack_chk_fail`); import stubs (`jmp [rip+got]`) resolved at the call site; jump tables (GCC/Clang PIC tables, hoisted and copied bases, `cmp/ja` and `cmp/jbe` guards, `and`-mask and bounded scans that stop at the next table); overlapping instruction streams |
| Semantics | ~600 mnemonics: integer, flags, partial registers, mul/div with #DE, string ops (`rep movs/stos`), `lock` atomics incl. bit strings, BMI1, MOVBE, pushfq/popfq, MXCSR, SSE–SSE4.1, AVX1 (VEX), masked moves; `rdtsc` and `cpuid` through runtime functions; `int n`, `syscall` and privileged instructions trap (as they fault natively) |
| Imports | ELF `GLOB_DAT`/`JUMP_SLOT`/`R_X86_64_64`/`RELATIVE`/`DT_RELR` relocations; imports bound by name to host functions (an explicit table first, `dlsym` second), called from recompiled code through a C bridge (`harness/recomp_loader.c`); per-thread guest stack and FS thread block (stack protector canary) |
| Tests | corpus: 144 functions (C integer, C FP/SIMD, and `flags.S`: flag-exact assembly tests) × 9 compiler/flag builds × 2 lifting modes = 18 x86-64 configurations, plus 36 AArch64 ones (each with and without x86 memory ordering): all 54 pass against the original code |
| Whole library | the system's **libz.so 1.3.2** (unmodified distribution build, FDE discovery only): 1,581 checks of checksums, compress2/uncompress at all levels, streaming deflate (window sizes, memLevel, strategies, raw/zlib/gzip, flush modes) and inflate (small output windows, corrupted streams): all identical to the original, both lifting modes, ~1M import calls into host libc |
| AArch64 | runs under QEMU user mode and **on a phone** (Galaxy S25 Ultra, Snapdragon 8 Elite, Android 17, over `adb`, `harness/device/run_device.sh`): the corpus passes in all 36 AArch64 configurations, replaying results recorded from the original code on x86-64; recompiled libz produces the same output digest on AArch64 as on x86-64 in every lifting mode. Multi-threaded ordering is not tested yet (QEMU on x86 runs ARM threads with x86's ordering; the tests are single-threaded) |
| x86 ordering cost (phone, libz test) | none 1.26 s; barriers 4.45 s (3.5×); **acquire/release with an inline 16-byte check (default `--tso`) 1.37 s (1.08×)**, at +43% code; acquire/release with the SIGBUS fallback instead 2.15 s (1.7×: 1.78M alignment faults) |
| The game | 162,959 FDE functions (+2 found from calls), 9.98M instructions, lifted in under a minute (8 cores); 3 instruction sites unsupported (junk next to the 24 decode failures, which the project's own analysis also counts: 22); 63,641 calls to imports resolved through their GOT slots; 3,915 jump tables; ~11,700 indirect jumps through pointers left to the dispatcher (tail calls). ~7.8 GiB of IR in 82 shards (`harness/build_game.sh`), **compiled for AArch64** (`--abi`, no `--tso`): 83 objects in 33 min on 4 cores (2.2 CPU-hours, 2.2 GiB peak per clang), **243 MiB of machine code, 6.1× the original 39.8 MiB**; one shard needs `-mstrict-align` to avoid an LLVM bug (below). Not yet run |
| Hybrid (x86-64) | original and recompiled code in one process, calling each other both ways (`harness/recomp_hybrid.{h,c}`, `recomp_hybrid_x86.S`): entry patches into recompiled functions, the dispatcher's fallback into original code; every register incl. the upper YMM halves and MXCSR carried across. Corpus (`runner -H`): all 18 x86-64 builds pass with all functions, either half and a third recompiled (72 configurations, up to ~870k crossings each); `calls.c`, `nonabi.S` and `midjump.S` cover deep alternating recursion, 256-bit vector arguments, float stack arguments, struct returns, callbacks, a non-ABI call returning results in every register and jumps into the middle of code. Deliberately broken thunks (YMM upper halves, r15 either way, vector save after a native return, jumps handled as calls) fail these tests. **In bbport (`BB_AOT`): the game's 198 hottest functions recompiled, in gameplay** (below) |
| Real code | zlib, lz4 and libpng (≈80,000 instructions) lift with no unsupported instruction and no CFG error; libzstd lifts and compiles (gaps: BMI2, which Jaguar lacks, and data inside its asm FDE ranges) |

## Use

```bash
python3 -m venv ../.venv-recomp && ../.venv-recomp/bin/pip install iced-x86 pyelftools
../.venv-recomp/bin/python -m recomp lift IMAGE.so -o out.ll [--fde] [--abi] [--tso]
clang -O2 --target=aarch64-linux-gnu -c out.ll -o out.o
harness/run.sh [FILTER]          # corpus differential tests (needs gcc, clang)
harness/run_zlib.sh [LIBZ.so]    # whole-library test against the system libz
```

Both scripts also run the AArch64 builds under QEMU when they find `zig` (0.13) and
`qemu-aarch64-static` (default `~/.local/opt/zig/zig` and
`~/.local/opt/qemu/usr/bin/qemu-aarch64-static`; override with `BB_ZIG` / `BB_QEMU`). The
corpus runner replays a record of the original code's results (`runner -r` on x86-64, `-c` on
AArch64); `zlib_test -d` runs only the recompiled code and prints a digest of every checked
output, which must equal the x86-64 run's.

- `--fde`: function ranges from `.eh_frame` instead of the symbol table.
- `--abi`: assume the SysV ABI for vector registers at calls (arguments xmm0-7, results xmm0-1,
  all caller-saved). 35-60% less code; the default mode assumes nothing.
- `--tso`: x86 memory ordering on AArch64 (`--tso-mode rcpc`, the default: `ldapr`/`stlr`, needs
  ARMv8.3 RCpc; `rcpc-fault`: no inline check, needs the runtime's SIGBUS handler; `barriers`:
  `dmb ishld` after loads and `dmb ishst` before stores, any ARMv8). No ordering for stack accesses
  (rsp, and rbp in frame-pointer functions; `--tso-strict` turns the rbp rule off) or for loads
  from read-only image segments.
- `recomp game EBOOT [--out DIR] [--report survey.json] [--abi] [--tso]`: the PS4 executable,
  surveyed (default) or written as shards.
- `--select MOD:REM` / `--only FILE` (lift): only a subset of the functions (index modulo MOD,
  or link addresses one per line); direct calls to the others go through `__recomp_dispatch`,
  which in hybrid execution runs the original code.

## Hybrid execution (x86-64)

- **Original → recompiled:** the function's first 5 bytes become `jmp rel32` to a 48-byte stub
  (within ±2 GiB) that leaves the recompiled function at `[rsp-0x18]` and jumps to
  `recomp_hybrid_enter`. That stores the registers into a per-thread `RecompState` (a stack of
  them: entries nest), switches to a host stack of its own (the lifted code addresses the guest
  stack at its real addresses, so LLVM's frames must not live there), calls the function, then
  loads the registers back and returns to the address the lifted `ret` popped. Scratch space is
  the red zone below the guest `rsp`; no register is clobbered.
- **Recompiled → original:** `__recomp_dispatch` with no recompiled target calls
  `recomp_dispatch_fallback` = `recomp_hybrid_call_native`, which swaps the guest's return slot
  for its own label, loads the registers, `ret`s into the target on the guest stack, stores the
  registers when the target returns there, and restores the slot. Imports go the same way (in
  bbport their GOT slots already hold host functions).
- **TLS:** bbport keeps the guest's thread pointer in GS (it rewrites the game's `fs:` accesses);
  `recomp_hybrid_fs_from_gs` makes the entry thunk give recompiled code (lifted from the
  unmodified executable, `fs:`) the GS base. Needs FSGSBASE (`rdfsbase`/`rdgsbase`, Linux 5.9+).
- **Jumps are not calls:** an indirect jump the CFG cannot resolve (`__recomp_dispatch_jump`), or
  a direct one into the middle of other code, has no return address at `[rsp]` to borrow. A
  recompiled function start is called (a tail call); anything else gets control for good:
  `recomp_hybrid_jump_native` ends the thread's innermost entry and jumps there with the guest's
  registers, and the original code returns along the guest's return addresses (original code's:
  recompiled code pushes the original return addresses). Borrowing `[rsp]` there overwrote a local
  in the game (a saved pointer to the stack guard came back as a return address: a crash on
  entering gameplay). `corpus/midjump.S` (`th_*`, hybrid-only) covers both kinds.
- **`--volatile`** (x86-64 hybrid builds): guest memory accesses (stack ones aside) as LLVM volatile
  loads and stores. The hardware keeps x86 ordering; LLVM can no longer drop or hoist them (a
  loop waiting on a flag another thread sets must keep reading it: the game hung at its first
  frames without it).
- **In bbport** (`src/runtime_aot.c`): `BB_AOT_DUMP=out/aot/live.bin` writes the game's code as it
  runs (after the port's patches and hooks); `harness/build_aot.sh EBOOT LIST LIVE NAME` lifts the
  listed functions whose live code equals the executable's (`fs`→`gs` prefixes aside; 80 of
  162,959 differ), with a hash of each, and builds `out/aot/NAME/libbbaot.so`; `BB_AOT=<it>`
  checks every hash (any difference: no start) and patches the entries before guest code runs.
  `BB_AOT_BISECT=lo:hi` swaps in only part of them. A fatal fault prints the thread's hybrid
  state and its last 24 calls and jumps into the original code.
- **setjmp / longjmp:** a recompiled call to the linked libc's setjmp becomes `__recomp_setjmp`
  (`returns_twice`): it fills the guest's jmp_buf as libc does and records, per thread and
  jmp_buf, how to resume the recompiled code (host return address, rsp, callee-saved registers,
  the entry depth and call records); libc's longjmp is replaced at its entry
  (`recomp_hybrid_longjmp`): a jmp_buf of a recompiled setjmp resumes it, any other gets libc's
  own behaviour. The signal mask is not saved (bbport delivers no guest signals). Callers of
  sigsetjmp, getcontext, swapcontext stay original: setjmp through the call thunk would keep
  the thunk's label as its return point. A longjmp in original
  code can still jump past recompiled frames and past calls into the original code that never
  return; the call thunk's records (the thread's `native_frame` chain) hold the slot each call
  borrowed and the entry depth at the call, and a return is matched to its record by the slot it
  came through: records and entries it skipped are dropped. (Entries skipped with no outer call
  coming back stay counted until the thread ends; in the game the depth stays at 4-8.)
- **Configurations:** settings install hooks of their own (`BB_FRAME_STATS`: one at 0x26aa860), so
  a library is built against one dump per configuration it must run in
  (`build_aot.sh EBOOT LIST dump1.bin,dump2.bin NAME`); bbport refuses it in any other.
- **Status in the game (2026-11-04):** gameplay, entering it from the title screen with
  `harness/desktop` keys: the 198 hottest functions (perf, 60 s of gameplay); the top 1,000 (959
  swapped in); all 4,273 functions the profile saw (4,034 swapped in: 105 too short or of unknown
  extent, 86 call setjmp/longjmp, 27 changed by bbport, 14 with a jump into their first 5 bytes, 5
  with CFG errors; `0x20858a0`, `0x2ab7350` stay original: bbport's write tracking knows them by
  address). 4 minutes: 799M entries, 2.08G calls back, 57M jumps back, no fault.
- **The whole game:** `recomp game --link out/link.json` loads the eboot with the game's linked
  system modules (libc.prx, libSceFios2.prx, at run.sh's bases; their setjmp/longjmp and callers
  stay original). Recompiled: 163,510 of 165,773 functions (14,836 of them for recompiled callers
  only: under 5 bytes or jumped into within 5); original: 2,001 setjmp/longjmp callers, 156 module
  functions with unsupported instructions, 81 changed by bbport, 21 CFG errors, 2 setjmp/longjmp.
- **Calls into bbport** (imports bound to host functions) take `recomp_hybrid_call_host`: argument
  and result registers only, on the guest stack (`BB_AOT_HOST_CALLS=full`: the whole register
  file). Two runtime functions look up the stack for the calling game code (the debug-exception
  handler that ignores the blur-kernel assertion, the unresolved-import report); with hybrid
  execution they find a thunk where the game's return address was: not handled yet.
- **Cost (gameplay, Ryzen 7 5800X, RTX 3070, ~80 FPS unmodified):** 4,034 functions 37 FPS (~14M
  full crossings a second); the whole eboot 44 FPS; with the modules and light calls into bbport
  45 FPS: the recompiled code itself was the cost then (66% of samples, the original code had
  31%: default mode stores and loads the whole register file at every call). **`--abi`
  (`BB_RECOMP_MODE=abi`): 70 FPS**, a 511 MiB library (default mode 1.16 GiB) built in 6.4 min;
  with longjmp callers recompiled too (only setjmp callers stay original): **79 FPS**, as fast as
  unmodified, 165,383 of 165,771 functions.
  Dispatch is a hash lookup, MXCSR is loaded only when it changed, counts are per thread (a
  shared atomic counter was a cache-line fight between cores).
- **Known limits:** C++ exceptions cannot unwind through the thunks (a native callee's return
  address is the thunk's label; recompiled frames have no unwind info), so functions with handlers
  and their callers stay original; patching a running function's entry is not atomic yet (install
  before the code runs); code that jumps into the first 5 bytes of a patched function is not
  detected; bbport classifies GPU-memory writes by the faulting instruction's address
  (`gpu/layer/layer_traps.cpp` `IsGuestCode`), and recompiled code is not the game's to it.

## Execution model

- One LLVM function per guest function, `void @F_<link address>(ptr noalias %st)`. `%st` is the
  register file (`harness/recomp_rt.h`, `STATE_TYPE` in `recomp/lift.py`).
- Registers live in allocas and are written back at calls, exits and returns; LLVM removes what
  is unused. Flags are function-local: not preserved across `call`/`ret`, as compiled code assumes.
- Guest memory is host memory at the guest address. Code addresses are link address +
  `__recomp_slide`. Calls push the guest return address (the game's stack layout is unchanged);
  `ret` pops it and returns to the LLVM caller.
- Indirect calls/jumps and calls to code that was not lifted go through `__recomp_dispatch`
  (sorted table `__recomp_table`, link address → function). A miss is where the planned
  fallback (embedded x86 JIT or interpreter) will plug in.
- Unsupported instructions and `int3`/`ud2` call `__recomp_trap`; the lift log lists them.

## Known deviations from x86

- **NaN sign and payload.** x86 returns the first operand's NaN and a negative default NaN;
  LLVM may commute `fadd`/`fmul` operands and AArch64 produces a positive default NaN. Results
  that are NaN stay NaN. The harness treats any NaN as equal for `tf_*` tests.
- `rcpps`/`rsqrtps` (approximations whose bits differ between x86 implementations) are exact.
- MXCSR rounding mode and DAZ/FTZ changes are not modelled yet (default round-to-nearest).
- Undefined flags are whatever the formula gives, not what a particular CPU leaves.
- `DF` (direction flag) is assumed clear, as the ABI requires at calls.
- **Locals are not shared**: with `--tso`, stack accesses (rsp-based, and rbp-based in functions with a
  frame pointer) get no ordering; a thread that publishes the address of a local to another thread
  and synchronises through it without atomics would see them unordered.
- MXCSR's exception status flags (bits 0-5) are not tracked: `stmxcsr` returns the control bits
  that were set and no sticky exception flags.
- Locked operations assume natural alignment (`align N` on the LLVM atomics). x86 allows
  misaligned locked operations (split locks); AArch64 before LSE2 faults on them. Compilers do not
  emit them for aligned C++ atomics.

## Not done yet (in order)

1. The game inside bbport, x86-64 first: link `libc.prx` and `libSceFios2.prx` (they are lifted from
   the image `scripts/link_modules.py` builds; today only `eboot.bin` is), bind the game's NID
   imports (661 GOT slots) to bbport's runtime, turn the guest-code hooks
   (`gpu/games/bloodborne/hooks.cpp`, `gpu/shim/bbport_guest_code_hooks.cpp`) into a recompiler
   input table, and run the recompiled x86-64 build in place of the original code (`BB_AOT=1`).
2. Multi-threaded tests of `lock` atomicity and of `--tso` ordering, on the phone (the only place
   ordering bugs can show).
3. Imports with float or stack arguments need per-function bridges (the generic bridge passes six
   integer registers). Callbacks from host code into guest code (e.g. `qsort` comparators) are not
   handled yet.
4. Fallback for dispatch misses; host-PC → guest-address map for fault reporting.
5. Code size: libz is 3.4× its original `.text` on AArch64 with `--abi` (flags and general
   register traffic are the next targets); `--tso` adds 43% for its inline alignment checks.
6. Instruction gaps: x87 (the game's handful of sites are junk decodes so far), 16-bit
   `shld/shrd`, BMI2 (not needed for Jaguar), segment register moves, `lahf/sahf`.
7. LLVM 23.1.4's AArch64 backend crashes ("Cannot select") on an unaligned 16-bit store it builds
   itself by merging two byte stores of a big-endian value
   (`harness/llvm-bug-store-merge-bswap16.ll`, 31 lines); report it upstream. Until it is fixed,
   `build_game.sh` retries such a shard with GlobalISel and then `-mstrict-align` (one of the
   game's 82 shards today; strict alignment makes its misaligned accesses byte-wise).
