// SPDX-License-Identifier: GPL-2.0-or-later
// String instructions with the direction flag set (std): a backward overlapping copy, as libc's
// memmove does it (std; rep movsb / movsq from the last element; cld). Signature as int_basic.c:
// a=rdi, b=rsi, c=rdx, buf=rcx (256 bytes).
    .intel_syntax noprefix
    .text

// buf[k + d .. k + d + n) = buf[k .. k + n), copied from the end (the ranges overlap for d < n):
// k = a & 63, d = 1 + (b & 31), n = c & 127.
    .globl t_movs_backward
    .type t_movs_backward, @function
    .p2align 4
t_movs_backward:
    mov r8, rcx                     // buf
    mov r9, rdi
    and r9, 63                      // k
    mov r10, rsi
    and r10, 31
    inc r10                         // d
    mov rcx, rdx
    and rcx, 127                    // n
    mov rax, rcx
    lea rsi, [r8 + r9 - 1]
    add rsi, rcx                    // the source's last byte
    lea rdi, [rsi + r10]            // the destination's last byte
    std
    rep movsb
    cld
    sub rdi, r8                     // where the pointers ended, relative to buf
    sub rsi, r8
    shl rdi, 16
    or rax, rdi
    shl rsi, 32
    or rax, rsi
    ret
    .size t_movs_backward, . - t_movs_backward

// The same with quadwords (rep movsq backwards) and a stosd backwards after it.
    .globl t_movsq_backward
    .type t_movsq_backward, @function
    .p2align 4
t_movsq_backward:
    mov r8, rcx
    mov rcx, rdx
    and rcx, 7                      // n quadwords
    mov r9, rdi
    and r9, 7
    lea rsi, [r8 + r9 * 8 + 64]     // the last source quadword: buf + 64 + 8k
    lea rdi, [rsi + 8]              // one quadword above: overlapping
    std
    rep movsq                       // rdi ends at buf + 72 + 8k - 8n >= buf + 16
    mov rax, rsi
    sub rax, r8                     // where the source pointer ended, relative to buf
    mov ecx, 2
    rep stosd                       // two dwords downwards from rdi: inside buf
    cld
    sub rdi, r8
    shl rdi, 32
    or rax, rdi
    ret
    .size t_movsq_backward, . - t_movsq_backward

    .section .note.GNU-stack, "", @progbits
