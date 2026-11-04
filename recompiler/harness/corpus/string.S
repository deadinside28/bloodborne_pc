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

// repe cmpsb / cmpsq, and a backward repe cmpsb: buf[0..n) against buf[d..d+n) (equal for a
// stretch when the bytes repeat), returning where the pointers and rcx stopped and the flags.
    .globl t_cmps
    .type t_cmps, @function
    .p2align 4
t_cmps:
    mov r8, rcx                     // buf
    mov r9, rdi
    and r9, 63                      // d
    mov rcx, rsi
    and rcx, 63                     // n
    mov rsi, r8
    lea rdi, [r8 + r9]
    repe cmpsb
    setz al
    setc ah
    movzx eax, ax
    sub rsi, r8
    shl rsi, 16
    or rax, rsi
    shl rcx, 32
    or rax, rcx
    lea rsi, [r8 + 64]              // quadwords: buf[64..) against buf[64 + 8 (d & 7)..)
    mov r10, r9
    and r10, 7
    lea rdi, [r8 + r10 * 8 + 64]
    mov rcx, rdx
    and rcx, 7
    repe cmpsq
    mov r11d, 0                     // (setz writes the low byte only; mov keeps the flags)
    setz r11b
    shl r11, 48
    xor rax, r11
    lea rsi, [r8 + 127]             // backwards from the end of buf[64..128)
    lea rdi, [r8 + r9 + 127]
    mov rcx, rdx
    shr rcx, 3
    and rcx, 63
    std
    repne cmpsb
    cld
    sub rdi, r8
    shl rdi, 52
    xor rax, rdi
    ret
    .size t_cmps, . - t_cmps

// lock cmpxchg16b on buf[16..32): the expected value is a:b, or buf's own when b is odd (the
// swap succeeds then); returns what rdx:rax hold after it and ZF.
    .globl t_cmpxchg16b
    .type t_cmpxchg16b, @function
    .p2align 4
t_cmpxchg16b:
    push rbx
    mov r8, rcx                     // buf (16-byte aligned: the runner's buffers)
    mov rax, rdi
    mov rdx, rsi
    test dl, 1
    jz 1f
    mov rax, [r8 + 16]              // c odd: expect what is there
    mov rdx, [r8 + 24]
1:  mov rbx, rdx
    xor rbx, rax                    // the new value
    mov rcx, rax
    not rcx
    lock cmpxchg16b [r8 + 16]
    setz bl
    movzx ebx, bl
    xor rax, rdx
    shl rbx, 63
    xor rax, rbx
    pop rbx
    ret
    .size t_cmpxchg16b, . - t_cmpxchg16b

    .section .note.GNU-stack, "", @progbits
