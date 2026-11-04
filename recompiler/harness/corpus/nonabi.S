// SPDX-License-Identifier: GPL-2.0-or-later
// A call that does not follow the ABI: the helper returns results in every general register
// (callee-saved ones included) and in all 256 bits of ymm0 and ymm1, and its caller uses them
// all. Hand-written code may do this; the recompiler's default mode transfers the whole register
// file at calls, and the hybrid thunks must carry every register in both directions. (Vector
// results beyond ymm0/ymm1 are left out: --abi reads only those back after a call.)
// t_nonabi_regs has int_basic.c's signature: a=rdi, b=rsi, c=rdx, buf=rcx.
    .intel_syntax noprefix
    .text

    .type nonabi_helper, @function
    .p2align 4
nonabi_helper:
    mov rax, rdi
    lea rbx, [rdi + rsi * 2 + 0x11]
    mov rcx, rsi
    xor rcx, rdx
    lea rbp, [rsi + rdx * 4 + 0x22]
    mov r8, rdi
    rol r8, 13
    mov r9, rsi
    ror r9, 7
    lea r10, [rdx + rdi * 8 + 0x33]
    mov r11, rdx
    imul r11, rdi
    mov r12, rsi
    not r12
    lea r13, [rdi + rsi + 0x44]
    mov r14, rdx
    rol r14, 29
    mov r15, rdi
    xor r15, rsi
    xor r15, 0x55
    lea rsi, [rdi + rdx * 2 + 0x66]
    imul rdi, rdi, 77
    lea rdx, [rdx + rax * 4 + 0x77]
    vmovq xmm0, rax
    vmovq xmm2, rbx
    vpunpcklqdq xmm0, xmm0, xmm2
    vmovq xmm3, rcx
    vmovq xmm4, rbp
    vpunpcklqdq xmm3, xmm3, xmm4
    vinsertf128 ymm0, ymm0, xmm3, 1
    vmovq xmm1, r8
    vmovq xmm2, r12
    vpunpcklqdq xmm1, xmm1, xmm2
    vmovq xmm3, r14
    vmovq xmm4, r15
    vpunpcklqdq xmm3, xmm3, xmm4
    vinsertf128 ymm1, ymm1, xmm3, 1
    ret
    .size nonabi_helper, . - nonabi_helper

    .globl t_nonabi_regs
    .type t_nonabi_regs, @function
    .p2align 4
t_nonabi_regs:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rcx                    // buf
    call nonabi_helper
    push rax
    mov rax, [rsp + 8]          // buf
    vmovdqu [rax], ymm0
    vmovdqu [rax + 32], ymm1
    mov [rax + 64], rbx
    mov [rax + 72], rcx
    mov [rax + 80], rdx
    mov [rax + 88], rbp
    mov [rax + 96], rsi
    mov [rax + 104], rdi
    mov [rax + 112], r8
    mov [rax + 120], r9
    mov [rax + 128], r10
    mov [rax + 136], r11
    mov [rax + 144], r12
    mov [rax + 152], r13
    mov [rax + 160], r14
    mov [rax + 168], r15
    pop rax
    imul rax, rax, 3
    add rax, r15
    xor rax, r11
    add rax, rsi
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    vzeroupper
    ret
    .size t_nonabi_regs, . - t_nonabi_regs

    .section .note.GNU-stack, "", @progbits
