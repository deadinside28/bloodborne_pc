// SPDX-License-Identifier: GPL-2.0-or-later
// The x87 environment (no register stack), as libc's fenv functions use it: the control word
// saved, changed, read back and restored; fnstenv (whose instruction pointers differ between the
// original and the recompiled code: only its control, status and tag words are kept); fnclex;
// fnstsw ax. Signature as int_basic.c: a=rdi, b=rsi, c=rdx, buf=rcx (256 bytes).
    .intel_syntax noprefix
    .text

    .globl t_x87_env
    .type t_x87_env, @function
    .p2align 4
t_x87_env:
    sub rsp, 40
    fnstcw [rsp]                    // the caller's control word
    mov ax, [rsp]
    mov [rcx], ax
    mov ax, di
    and ax, 0x0f3f                  // rounding, precision and exception masks from a
    or ax, 0x0040                   // bit 6 reads as 1
    mov [rsp + 2], ax
    fldcw [rsp + 2]
    fnstcw [rcx + 2]
    fnstenv [rsp + 8]               // 28 bytes; masks every exception afterwards
    mov ax, [rsp + 8]
    mov [rcx + 4], ax               // its control word
    mov ax, [rsp + 12]
    mov [rcx + 6], ax               // its status word
    mov ax, [rsp + 16]
    mov [rcx + 8], ax               // its tag word (empty: 0xffff)
    fnstcw [rcx + 10]               // the control word after fnstenv
    fnclex
    fnstsw ax
    mov [rcx + 12], ax
    fldcw [rsp]                     // the caller's again
    movzx eax, word ptr [rcx + 2]
    add rsp, 40
    ret
    .size t_x87_env, . - t_x87_env

    .section .note.GNU-stack, "", @progbits
