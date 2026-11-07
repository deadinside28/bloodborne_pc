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
    fninit                          // the status word: what ran before (x87.c) leaves flags and C1
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

// fxsave as libc's unwinder (unw_getcontext) uses it, and the segment registers it records: the
// control, status and abridged tag words, ST0-1, MXCSR and two XMM registers (FOP/FIP/FDP name
// code addresses and MXCSR_MASK is the CPU's: not compared); mov from cs and ss.
    .globl t_fxsave
    .type t_fxsave, @function
    .p2align 4
t_fxsave:
    push rbp
    mov rbp, rsp
    sub rsp, 528
    and rsp, -16
    fninit
    fld1
    fldpi                           // ST0 = pi, ST1 = 1: two tags in use
    movq xmm0, rdi
    movq xmm1, rsi
    punpcklqdq xmm0, xmm1           // xmm0 = a:b
    movq xmm15, rdx                 // xmm15 = c
    fxsave [rsp]
    mov eax, [rsp]                  // FCW, FSW
    mov [rcx], eax
    mov al, [rsp + 4]               // abridged tags
    mov [rcx + 4], al
    mov eax, [rsp + 24]             // MXCSR, without the sticky exception flags (not tracked:
    and eax, -64                    // README, known deviations)
    mov [rcx + 8], eax
    movups xmm2, [rsp + 32]         // ST0 (10 bytes, then 6 reserved)
    movups [rcx + 16], xmm2
    movups xmm2, [rsp + 48]         // ST1
    movups [rcx + 32], xmm2
    movups xmm2, [rsp + 160]        // XMM0
    movups [rcx + 48], xmm2
    movups xmm2, [rsp + 160 + 15 * 16]  // XMM15
    movups [rcx + 64], xmm2
    mov ax, cs
    mov [rcx + 80], ax
    mov [rcx + 82], ss
    mov eax, ss
    mov [rcx + 84], eax
    pushfq                          // the flags saved as unw_getcontext does: pop to memory
    pop qword ptr [rcx + 88]
    and qword ptr [rcx + 88], -17   // AF: undefined after the last flag writer (and)
    push rdi
    pop qword ptr [rsp]             // rsp-based: the address uses rsp after the pop
    mov rax, [rsp]
    mov [rcx + 96], rax
    fninit
    movzx eax, word ptr [rcx + 2]
    leave
    ret
    .size t_fxsave, . - t_fxsave

    .section .note.GNU-stack, "", @progbits
