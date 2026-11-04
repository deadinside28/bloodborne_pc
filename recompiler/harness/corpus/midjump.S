// SPDX-License-Identifier: GPL-2.0-or-later
// Hybrid-only tests (th_*, runner -H): jumps the CFG cannot resolve into the middle of code. In
// hybrid execution __recomp_dispatch_jump hands control to the original code there for good;
// [rsp] is a local at such a jump, not a return address, and must not be touched (the game
// crashed when the call path borrowed it: a pointer the function had saved there came back as
// the thread's return address). Without the original code next to it (the plain runs, AArch64)
// the lifted code has nowhere to go: those runs record these tests and skip them.
// Signature as int_basic.c: a=rdi, b=rsi, c=rdx, buf=rcx.
    .intel_syntax noprefix

    .data
    .p2align 3
midjump_targets:
    .quad .Lmid_self
    .quad .Lmid_other

    .text
// Jumps into its own middle with a saved register at [rsp].
    .globl th_midjump_self
    .type th_midjump_self, @function
    .p2align 4
th_midjump_self:
    push rbx
    mov rbx, rdi
    imul rbx, rsi                  // a callee-saved register with a value: [rsp] holds the caller's rbx
    and rdx, 0                     // index 0, opaque to the CFG
    lea rax, [rip + midjump_targets]
    jmp qword ptr [rax + rdx * 8]
    ud2
.Lmid_self:
    lea rax, [rbx + rdi * 2]
    add rax, rbx
    pop rbx                        // the caller's rbx, from [rsp]: runner -H checks it is intact
    ret
    .size th_midjump_self, . - th_midjump_self

// Jumps into the middle of another function, which returns for it.
    .type midjump_helper, @function
    .p2align 4
midjump_helper:
    push rbp
    mov rbp, rdi
    imul rbp, rdi
.Lmid_other:
    lea rax, [rbp + rsi * 4]
    pop rbp
    ret
    .size midjump_helper, . - midjump_helper

    .globl th_midjump_other
    .type th_midjump_other, @function
    .p2align 4
th_midjump_other:
    push rbp                       // midjump_helper's frame: its pop restores the caller's rbp
    lea rbp, [rdi + 7]
    mov rax, rdx
    and rax, 0
    or rax, 1                      // index 1, opaque to the CFG
    lea rdx, [rip + midjump_targets]
    jmp qword ptr [rdx + rax * 8]
    .size th_midjump_other, . - th_midjump_other

    .section .note.GNU-stack, "", @progbits
