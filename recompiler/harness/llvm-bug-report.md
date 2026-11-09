# Draft: LLVM issue for the bug tracker (not filed)

**Title:** [AArch64][SelectionDAG] "Cannot select" on an unaligned bswap'd i16 store formed by DAG combining

**Labels:** backend:AArch64, llvm:SelectionDAG, crash

## Summary

`llc` aborts in AArch64 instruction selection with `LLVM ERROR: Cannot select` on a truncating
store of an `i16` `bswap`, with align 1, that DAG combining forms from two byte stores of
`(v >> 8)` and `v`. No such store is present in the IR.

## Reproduce

Input: `llvm-bug-store-merge-bswap16.ll` (attached, reduced with llvm-reduce).

    llc -mtriple=aarch64-linux-gnu -O2 -o /dev/null llvm-bug-store-merge-bswap16.ll

or

    clang -O2 --target=aarch64-linux-gnu -c llvm-bug-store-merge-bswap16.ll -o /dev/null

## Output

    LLVM ERROR: Cannot select: ch = store<(store (s16) into %ir.x7, align 1)> ..., ..., ..., undef:i64
      i16 = bswap ...
        i16 = truncate ...
          i64,ch = load<(load (s64) from %ir.st, align 4)> ...
    In function: f

    Running pass 'AArch64 Instruction Selection' on function '@f'

## What triggers it (each line tested with llc 22.1.8)

| Options | Result |
| --- | --- |
| `-mtriple=aarch64-linux-gnu -O2` | crash |
| `-mtriple=aarch64-linux-gnu -O1` | crash |
| `-mtriple=arm64-apple-macos -O2` | crash |
| `-mtriple=aarch64-linux-gnu -O2 -combiner-store-merging=false` | crash |
| `-mtriple=aarch64-linux-gnu -O0` | compiles |
| `-mtriple=aarch64-linux-gnu -O2 -global-isel` | compiles |
| `-mtriple=aarch64-linux-gnu -O2 -mattr=+strict-align` | compiles |
| `-mtriple=aarch64_be-linux-gnu -O2` | compiles |
| `-mtriple=x86_64-linux-gnu -O2` | compiles |

Because `-combiner-store-merging=false` still crashes, the store appears to come from a combine
other than the usual store-merging path. I have not identified which one.

## Versions

- Fedora `clang version 22.1.8 (Fedora 22.1.8-4.fc44)`: reproduced with the commands above.
- The test case's header records LLVM 23.1.4, where it was first reduced. Before filing, re-check
  against current `main` (Compiler Explorer's trunk is enough).

## Workaround in use

Retry the affected module with GlobalISel (`-fglobal-isel -mllvm -global-isel-abort=1`), and
failing that with `-mstrict-align`.

## Notes for whoever files this

- Attach `llvm-bug-store-merge-bswap16.ll`. Its first comment lines describe the same facts.
- Drafted for a person to file; nothing has been posted.
