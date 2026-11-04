; LLVM 23.1.4 AArch64 SelectionDAG: "Cannot select" on an unaligned 16-bit store that DAG
; combining builds itself from two byte stores of (v >> 8) and v (a big-endian 16-bit store).
; Reproduce: clang -O2 --target=aarch64-linux-gnu -c llvm-bug-store-merge-bswap16.ll
; Not affected: -fglobal-isel, -mstrict-align, x86-64. -combiner-store-merging=false does not help.
; Reduced (llvm-reduce) from recompiled code; build_game.sh retries such shards with GlobalISel.


define void @f(ptr %st, ptr %base, i1 %cond) {
entry:
  %x0 = load i64, ptr %base, align 4
  br i1 %cond, label %bb0, label %bb1

common.ret:                                       ; preds = %bb1, %bb0
  ret void

bb0:                                         ; preds = %entry
  store i64 %x0, ptr %st, align 1
  br label %common.ret

bb1:                                         ; preds = %entry
  %x1 = load i64, ptr %st, align 4
  %0 = lshr i64 %x1, 8
  %x2 = trunc i64 %0 to i8
  store i8 %x2, ptr %base, align 1
  %x3 = trunc i64 %x1 to i8
  store i8 %x3, ptr %st, align 1
  %x4 = add i64 %x0, 1
  %x5 = load i64, ptr %st, align 1
  %x6 = add i64 %x4, %x5
  %x7 = inttoptr i64 %x6 to ptr
  store i8 %x2, ptr %x7, align 1
  %x8 = add i64 %x0, 2
  %x9 = add i64 %x8, %x5
  %x10 = inttoptr i64 %x9 to ptr
  store i8 %x3, ptr %x10, align 1
  br label %common.ret
}
