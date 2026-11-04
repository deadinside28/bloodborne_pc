// SPDX-License-Identifier: GPL-2.0-or-later
// Hybrid execution thunks (see recomp_hybrid.h). x86-64 only.
//
// RecompState (recomp_rt.h): gpr[16] at 0 (rax rcx rdx rbx rsp rbp rsi rdi r8..r15), rip 128,
// fs_base 136, gs_base 144, mxcsr 152, xmm[16] at 160, ymmh[16] at 416; 672 bytes.
// HybridThread: host_sp 0, depth 8, states 16, max_depth 24, native_frame 32, entries 40.
//
// No guest register is clobbered in either direction. Scratch space for the thunks is the guest's
// red zone below rsp, which belongs to the function being entered (or, after a return, to the
// caller, which cannot hold anything there across a call).
        .intel_syntax noprefix
        .text

#define ST_SIZE 672

        .macro SAVE_VEC base
        stmxcsr dword ptr [\base + 152]
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        vmovdqu xmmword ptr [\base + 160 + 16 * \i], xmm\i
        vextractf128 xmmword ptr [\base + 416 + 16 * \i], ymm\i, 1
        .endr
        .endm

        // MXCSR only when it differs: ldmxcsr is slow, and the guest's value rarely changes.
        // The state's pad word (156) takes the current value; rcx is free (loaded after this).
        .macro LOAD_VEC base
        stmxcsr dword ptr [\base + 156]
        mov ecx, [\base + 156]
        cmp ecx, [\base + 152]
        je 77f
        ldmxcsr dword ptr [\base + 152]
77:
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        vmovdqu xmm\i, xmmword ptr [\base + 160 + 16 * \i]
        vinsertf128 ymm\i, ymm\i, xmmword ptr [\base + 416 + 16 * \i], 1
        .endr
        .endm

        // reg := this thread's HybridThread*
        .macro SELF reg
        mov \reg, qword ptr [rip + recomp_hybrid_self@GOTTPOFF]
        mov \reg, qword ptr fs:[\reg]
        .endm

// Restores the guest registers from the state in rax and returns to the address at
// [state.rsp - 8], leaving rsp = state.rsp: after a recompiled function's `ret` (which popped its
// return address) and to enter a native target placed in that slot.
        .p2align 4
        .type recomp_hybrid_resume, @function
recomp_hybrid_resume:
        LOAD_VEC rax
        mov rcx, [rax + 8]
        mov rdx, [rax + 16]
        mov rbx, [rax + 24]
        mov rbp, [rax + 40]
        mov rsi, [rax + 48]
        mov rdi, [rax + 56]
        mov r8,  [rax + 64]
        mov r9,  [rax + 72]
        mov r10, [rax + 80]
        mov r11, [rax + 88]
        mov r12, [rax + 96]
        mov r13, [rax + 104]
        mov r14, [rax + 112]
        mov r15, [rax + 120]
        mov rsp, [rax + 32]
        sub rsp, 8
        mov rax, [rax]
        ret
        .size recomp_hybrid_resume, . - recomp_hybrid_resume

// Original code -> recompiled function. Reached from a function's stub with the guest's
// registers live, [rsp] = return address, [rsp - 0x18] = the recompiled function.
        .globl recomp_hybrid_enter
        .p2align 4
        .type recomp_hybrid_enter, @function
recomp_hybrid_enter:
        mov [rsp - 0x10], rax
        mov [rsp - 0x20], rcx
        SELF rax
        test rax, rax
        jz 8f                           // first entry on this thread
        mov rcx, [rax + 8]
        cmp rcx, [rax + 24]
        jae 9f                          // nested too deep
        inc qword ptr [rax + 8]
        inc qword ptr [rax + 40]
        imul rcx, rcx, ST_SIZE
        add rcx, [rax + 16]             // rcx = state
        mov [rcx + 16], rdx
        mov [rcx + 24], rbx
        mov [rcx + 32], rsp
        mov [rcx + 40], rbp
        mov [rcx + 48], rsi
        mov [rcx + 56], rdi
        mov [rcx + 64], r8
        mov [rcx + 72], r9
        mov [rcx + 80], r10
        mov [rcx + 88], r11
        mov [rcx + 96], r12
        mov [rcx + 104], r13
        mov [rcx + 112], r14
        mov [rcx + 120], r15
        mov rdx, [rsp - 0x10]
        mov [rcx], rdx                  // rax
        mov rdx, [rsp - 0x20]
        mov [rcx + 8], rdx              // rcx
        mov rdx, [rsp]
        mov [rcx + 128], rdx            // rip: the return address (for reports)
        rdfsbase rdx
        rdgsbase rbx
        mov [rcx + 144], rbx
        mov r8, [rip + recomp_hybrid_fs_from_gs@GOTPCREL]
        cmp dword ptr [r8], 0
        cmovne rdx, rbx
        mov [rcx + 136], rdx
        SAVE_VEC rcx
        mov r11, [rsp - 0x18]           // the recompiled function
        mov rbx, rax                    // callee-saved across the call: thread, state
        mov r12, rcx
        mov rsp, [rax]                  // the host stack
        cld
        mov rdi, rcx
        call r11
        dec qword ptr [rbx + 8]
        mov rax, r12
        jmp recomp_hybrid_resume
        // No HybridThread yet (a thread that did not call recomp_hybrid_thread_init, e.g. a host
        // thread calling back into the game): make one in C, on the guest stack below the slots
        // above, with the argument registers kept; then enter again.
8:      mov rax, [rsp - 0x10]
        lea rsp, [rsp - 0x28]
        push rdi
        push rsi
        push rdx
        push rcx
        push r8
        push r9
        push r10
        push r11
        push rax                        // rsp = entry rsp - 112: 8 mod 16, as at the entry
        sub rsp, 264                    // ymm0-7 and the result; 16-byte aligned for the call
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7
        vmovdqu ymmword ptr [rsp + 32 * \i], ymm\i
        .endr
        call recomp_hybrid_thread_init@PLT
        mov [rsp + 256], eax
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7
        vmovdqu ymm\i, ymmword ptr [rsp + 32 * \i]
        .endr
        cmp dword ptr [rsp + 256], 0
        lea rsp, [rsp + 264]
        pop rax
        pop r11
        pop r10
        pop r9
        pop r8
        pop rcx
        pop rdx
        pop rsi
        pop rdi
        lea rsp, [rsp + 0x28]
        jne 9f
        jmp recomp_hybrid_enter
9:      ud2                             // nested too deep, or no memory for the thread's state
        .size recomp_hybrid_enter, . - recomp_hybrid_enter

// Recompiled code -> original code: void recomp_hybrid_call_native(RecompState *st, uint64_t target).
// The guest has pushed its return address at [st.rsp] (call) or holds its caller's there (tail
// call); the target's `ret` comes back here instead, and the slot is restored afterwards.
// Host frame, a record in the thread's native_frame chain (rsp after the pushes): [0] state,
// [8] previous native_frame, [16] the guest's return slot value, [24] previous host_sp, [32] the
// slot's address, [40] the thread's entry depth at the call.
// longjmp: original code may jump past recompiled frames and past calls made here that never
// return (a setjmp in original code, a longjmp below recompiled code). The return that comes
// here is then an outer call's: the record is found by the slot the return came through, the
// inner ones are dropped with the host stack below it, and the depth goes back to the call's.
        .globl recomp_hybrid_call_native
        .p2align 4
        .type recomp_hybrid_call_native, @function
recomp_hybrid_call_native:
        push rbp
        push rbx
        push r12
        push r13
        push r14
        push r15
        sub rsp, 56
        mov [rsp], rdi
        SELF rax
        mov rcx, [rax + 32]
        mov [rsp + 8], rcx
        mov rcx, [rax]
        mov [rsp + 24], rcx
        mov rcx, [rax + 8]
        mov [rsp + 40], rcx
        mov [rax + 32], rsp
        lea rcx, [rsp - 256]            // entries nested in the target continue below this frame
        and rcx, -16
        mov [rax], rcx
        mov rdx, [rdi + 32]             // guest rsp
        mov [rsp + 32], rdx
        mov rcx, [rdx]
        mov [rsp + 16], rcx
        lea rcx, [rip + 1f]
        mov [rdx], rcx                  // the target returns to 1:
        mov [rdx - 8], rsi              // recomp_hybrid_resume "returns" into the target
        mov rax, rdi
        jmp recomp_hybrid_resume
1:      mov [rsp - 8], rax              // guest registers live, rsp = guest rsp after the ret
        mov [rsp - 16], rcx
        SELF rax
        mov rax, [rax + 32]             // the innermost record
        lea rcx, [rsp - 8]              // the slot this return came through
2:      test rax, rax
        jz 9f
        cmp rcx, [rax + 32]
        je 3f
        mov rax, [rax + 8]              // not this one: left behind by a longjmp
        jmp 2b
3:      mov [rsp - 24], rax
        mov rax, [rax]                  // state
        mov rcx, [rsp - 16]
        mov [rax + 8], rcx
        mov [rax + 16], rdx
        mov [rax + 24], rbx
        mov [rax + 32], rsp
        mov [rax + 40], rbp
        mov [rax + 48], rsi
        mov [rax + 56], rdi
        mov [rax + 64], r8
        mov [rax + 72], r9
        mov [rax + 80], r10
        mov [rax + 88], r11
        mov [rax + 96], r12
        mov [rax + 104], r13
        mov [rax + 112], r14
        mov [rax + 120], r15
        mov rcx, [rsp - 8]
        mov [rax], rcx
        SAVE_VEC rax
        mov rbx, [rsp - 24]             // the record
        SELF rdx
        mov rsp, rbx                    // back on its host frame (anything below is dropped)
        mov rcx, [rsp + 8]
        mov [rdx + 32], rcx             // native_frame
        mov rcx, [rsp + 24]
        mov [rdx], rcx                  // host_sp
        mov rcx, [rsp + 40]
        mov [rdx + 8], rcx              // depth
        mov rdx, [rsp + 32]
        mov rcx, [rsp + 16]
        mov [rdx], rcx                  // the guest's return slot as it was
        cld
        add rsp, 56
        pop r15
        pop r14
        pop r13
        pop r12
        pop rbx
        pop rbp
        ret
9:      ud2                             // a return through a slot no call here borrowed
        .size recomp_hybrid_call_native, . - recomp_hybrid_call_native

// Recompiled code -> a host function (an import bound to the runtime, which follows the SysV
// ABI): void recomp_hybrid_call_host(RecompState *st, uint64_t target). Much lighter than
// recomp_hybrid_call_native: only the argument registers are loaded (rdi rsi rdx rcx r8 r9,
// xmm0-7, and rax: al counts vector arguments of varargs calls) and only the results stored
// (rax rdx xmm0 xmm1); the callee keeps the callee-saved registers, whose guest values stay in
// the state. It runs on the guest stack just above the return slot the guest pushed, so stack
// arguments are where it expects them; its own return address borrows that slot for the call.
// Entries into recompiled code from the host function (callbacks) continue below this frame.
        .globl recomp_hybrid_call_host
        .p2align 4
        .type recomp_hybrid_call_host, @function
recomp_hybrid_call_host:
        push rbp
        push rbx
        push r12
        push r13
        push r14
        push r15
        sub rsp, 8
        mov rbx, rdi                    // state
        mov r12, rsi                    // target
        SELF r13
        mov r14, [r13]                  // host_sp, restored afterwards
        lea rax, [rsp - 256]
        and rax, -16
        mov [r13], rax
        mov r15, rsp                    // this frame
        mov rax, [rbx + 32]
        mov rbp, [rax]                  // the guest's return address
        mov rdi, [rbx + 56]
        mov rsi, [rbx + 48]
        mov rdx, [rbx + 16]
        mov rcx, [rbx + 8]
        mov r8,  [rbx + 64]
        mov r9,  [rbx + 72]
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7
        vmovdqu xmm\i, xmmword ptr [rbx + 160 + 16 * \i]
        .endr
        lea rsp, [rax + 8]              // stack arguments at [rsp + 8] after the call
        mov rax, [rbx]
        call r12
        mov rsp, r15
        mov [rbx], rax
        mov [rbx + 16], rdx
        vmovdqu xmmword ptr [rbx + 160], xmm0
        vmovdqu xmmword ptr [rbx + 176], xmm1
        mov rax, [rbx + 32]
        mov [rax], rbp                  // the guest's return slot as it was
        add qword ptr [rbx + 32], 8     // the host function's return popped it
        mov [r13], r14
        cld
        add rsp, 8
        pop r15
        pop r14
        pop r13
        pop r12
        pop rbx
        pop rbp
        ret
        .size recomp_hybrid_call_host, . - recomp_hybrid_call_host

// void __recomp_setjmp(RecompState *st): the game libc's setjmp for recompiled callers, called
// returns_twice from the lifted code (recomp_hybrid.h). recomp_hybrid_setjmp_record fills the
// guest's jmp_buf and the thread's record; here the host side goes in: the return address into
// the recompiled code, its rsp after the return and the callee-saved registers, which
// recomp_hybrid_longjmp_resume restores to return here again. The first return gives 0.
        .globl __recomp_setjmp
        .p2align 4
        .type __recomp_setjmp, @function
__recomp_setjmp:
        push rdi                        // and aligns the call below
        call recomp_hybrid_setjmp_record@PLT
        pop rdi
        mov rcx, [rsp]
        mov [rax], rcx                  // host_ret
        lea rcx, [rsp + 8]
        mov [rax + 8], rcx              // host_rsp
        mov [rax + 16], rbx
        mov [rax + 24], rbp
        mov [rax + 32], r12
        mov [rax + 40], r13
        mov [rax + 48], r14
        mov [rax + 56], r15
        mov qword ptr [rdi], 0          // the guest's rax: setjmp returns 0
        ret
        .size __recomp_setjmp, . - __recomp_setjmp

// void recomp_hybrid_longjmp_resume(const HybridJmp *j): __recomp_setjmp returns again.
        .globl recomp_hybrid_longjmp_resume
        .p2align 4
        .type recomp_hybrid_longjmp_resume, @function
recomp_hybrid_longjmp_resume:
        mov rbx, [rdi + 16]
        mov rbp, [rdi + 24]
        mov r12, [rdi + 32]
        mov r13, [rdi + 40]
        mov r14, [rdi + 48]
        mov r15, [rdi + 56]
        mov rsp, [rdi + 8]
        cld
        jmp qword ptr [rdi]
        .size recomp_hybrid_longjmp_resume, . - recomp_hybrid_longjmp_resume

// Recompiled code -> original code for good: void recomp_hybrid_jump_native(RecompState *st,
// uint64_t target), no return. For jumps (not calls) to code that has no recompiled function: the
// middle of a function, say, where [rsp] is no return address to borrow. The thread's innermost
// entry ends here (its recompiled frames on the host stack are dropped); the original code
// continues at target with the guest's registers and stack, and returns along the guest's return
// addresses, which are original code's (recompiled code pushes the original return addresses).
// The jump goes through a thread variable: no guest register, and nothing below the guest rsp
// (the red zone of the code jumped into), is touched.
        .globl recomp_hybrid_jump_native
        .p2align 4
        .type recomp_hybrid_jump_native, @function
recomp_hybrid_jump_native:
        SELF rax
        dec qword ptr [rax + 8]
        mov qword ptr fs:recomp_hybrid_jump_target@tpoff, rsi
        mov rax, rdi
        LOAD_VEC rax
        mov rcx, [rax + 8]
        mov rdx, [rax + 16]
        mov rbx, [rax + 24]
        mov rbp, [rax + 40]
        mov rsi, [rax + 48]
        mov rdi, [rax + 56]
        mov r8,  [rax + 64]
        mov r9,  [rax + 72]
        mov r10, [rax + 80]
        mov r11, [rax + 88]
        mov r12, [rax + 96]
        mov r13, [rax + 104]
        mov r14, [rax + 112]
        mov r15, [rax + 120]
        mov rsp, [rax + 32]
        mov rax, [rax]
        jmp qword ptr fs:recomp_hybrid_jump_target@tpoff
        .size recomp_hybrid_jump_native, . - recomp_hybrid_jump_native

// uint64_t recomp_hybrid_checked_call(fn, a, b, c, d, fa, fb, uint64_t *xmm0, int *bad)
// rdi fn, rsi a, rdx b, rcx c, r8 d, r9 fa; stack: fb, xmm0, bad.
        .globl recomp_hybrid_checked_call
        .p2align 4
        .type recomp_hybrid_checked_call, @function
recomp_hybrid_checked_call:
        push rbp
        push rbx
        push r12
        push r13
        push r14
        push r15
        sub rsp, 24                     // stack arguments now at rsp + 80, 88, 96
        mov [rsp], rsp
        mov r11, rdi
        mov rdi, rsi
        mov rsi, rdx
        mov rdx, rcx
        mov rcx, r8
        vmovq xmm0, r9
        vmovq xmm1, qword ptr [rsp + 80]
        movabs rbx, 0x5a5a0000c0de0003
        movabs rbp, 0x5a5a0000c0de0005
        movabs r12, 0x5a5a0000c0de000c
        movabs r13, 0x5a5a0000c0de000d
        movabs r14, 0x5a5a0000c0de000e
        movabs r15, 0x5a5a0000c0de000f
        call r11
        movabs r11, 0x5a5a0000c0de0003
        mov r10, rbx
        xor r10, r11
        movabs r11, 0x5a5a0000c0de0005
        mov rcx, rbp
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000c
        mov rcx, r12
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000d
        mov rcx, r13
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000e
        mov rcx, r14
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000f
        mov rcx, r15
        xor rcx, r11
        or r10, rcx
        mov rcx, rsp
        xor rcx, [rsp]
        or r10, rcx
        mov rcx, [rsp + 88]
        vmovq qword ptr [rcx], xmm0
        mov rcx, [rsp + 96]
        test r10, r10
        setne r11b
        movzx r11d, r11b
        mov [rcx], r11d
        add rsp, 24
        pop r15
        pop r14
        pop r13
        pop r12
        pop rbx
        pop rbp
        ret
        .size recomp_hybrid_checked_call, . - recomp_hybrid_checked_call

        .section .note.GNU-stack, "", @progbits
