# recompiler: x86-64 → LLVM IR → x86-64 / AArch64

Static (ahead-of-time) recompiler for the game's x86-64 code: Phase 1 of
[docs/ANDROID_ARM64_PORT_PLAN.md](../docs/ANDROID_ARM64_PORT_PLAN.md). Not connected to bbport
yet, and not yet run on the game's eboot.

## Status

| | |
|---|---|
| Inputs | ELF64 x86-64 images; functions from symbols or from `.eh_frame` FDEs (the game's only source) |
| Control flow | recursive descent per function; tail calls; jump tables (GCC/Clang PIC tables, hoisted bases, `cmp/ja`, `and`-mask and range-scan bounds); overlapping instruction streams |
| Semantics | ~580 mnemonics: integer, flags, partial registers, mul/div with #DE, string ops (`rep movs/stos`), `lock` atomics incl. bit strings, BMI1, MOVBE, pushfq/popfq, SSE–SSE4.1, AVX1 (VEX), masked moves |
| Imports | ELF `GLOB_DAT`/`JUMP_SLOT`/`R_X86_64_64`/`RELATIVE`/`DT_RELR` relocations; imports bound to host functions (`dlsym`), called from recompiled code through a C bridge (`harness/recomp_loader.c`); guest FS thread block for the stack protector |
| Tests | corpus: 136 functions (C integer, C FP/SIMD, and `flags.S`: flag-exact assembly tests) × 9 compiler/flag builds × 2 lifting modes = 18 configurations, all passing against the original code on x86-64 |
| Whole library | the system's **libz.so 1.3.2** (unmodified distribution build, FDE discovery only): 1,581 checks of checksums, compress2/uncompress at all levels, streaming deflate (window sizes, memLevel, strategies, raw/zlib/gzip, flush modes) and inflate (small output windows, corrupted streams): all identical to the original, both lifting modes, ~1M import calls into host libc |
| AArch64 | every lifted module (corpus, libz, lz4, libpng, zstd) compiles for AArch64 (`-march=armv8.2-a+rcpc`), with and without x86 memory ordering; **not yet executed on ARM**, and the C runtime/drivers not yet compiled for it (no QEMU, no AArch64 sysroot on the dev machine) |
| Real code | zlib, lz4 and libpng (≈80,000 instructions) lift with no unsupported instruction and no CFG error; libzstd lifts and compiles (gaps: BMI2, which Jaguar lacks, and data inside its asm FDE ranges) |

## Use

```bash
python3 -m venv ../.venv-recomp && ../.venv-recomp/bin/pip install iced-x86 pyelftools
../.venv-recomp/bin/python -m recomp lift IMAGE.so -o out.ll [--fde] [--abi] [--tso]
clang -O2 --target=aarch64-linux-gnu -c out.ll -o out.o
harness/run.sh [FILTER]          # corpus differential tests (needs gcc, clang)
harness/run_zlib.sh [LIBZ.so]    # whole-library test against the system libz
```

- `--fde`: function ranges from `.eh_frame` instead of the symbol table.
- `--abi`: assume the SysV ABI for vector registers at calls (arguments xmm0-7, results xmm0-1,
  all caller-saved). 35-60% less code; the default mode assumes nothing.
- `--tso`: x86 memory ordering on weakly ordered targets (`fence acquire` after each non-stack
  load, `fence release` before each non-stack store → `dmb ishld` / `dmb ish` on AArch64).

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
- Locked operations assume natural alignment (`align N` on the LLVM atomics). x86 allows
  misaligned locked operations (split locks); AArch64 before LSE2 faults on them. Compilers do not
  emit them for aligned C++ atomics.

## Not done yet (in order)

1. Run the AArch64 build: install `qemu-user` + an AArch64 sysroot/linker (`lld` or
   `aarch64-linux-gnu-gcc`), then `runner -c expected-*.bin` replays the x86 results on ARM.
2. Imports with float or stack arguments need per-function bridges (the generic bridge passes six
   integer registers); for the game, NID imports (701) bind to bbport's runtime functions. Callbacks
   from host code into guest code (e.g. `qsort` comparators) are not handled yet.
3. A multi-threaded test of the `lock` paths (atomicity is invisible to the single-threaded runner;
   `recomp_call` uses one static guest stack).
4. The game: a loader for the linked probe image (`BBPROBE5`, `scripts/link_modules.py`), FDE
   ranges from `.eh_frame_hdr`, guest-code hooks as a recompiler input table
   (`gpu/games/bloodborne/hooks.cpp`), then the x86-64 AOT build inside bbport (`BB_AOT=1`).
5. Fallback for dispatch misses; host-PC → guest-address map for fault reporting.
6. Code size: libz is 3.4× its original `.text` on AArch64 with `--abi` (flags and general
   register traffic are the next targets).
7. Instruction gaps: x87 (if the game has any), 16-bit `shld/shrd`, BMI2 (not needed for
   Jaguar), segment register moves, `lahf/sahf`.
