.text
.p2align 4, 0x90
.globl _lp32_import_gateway
_lp32_import_gateway:
    /* r14 carries the high host stack while compatibility code is active. */
    xchgq %r14, %rsp

    /* i386 requires ebx, ebp, esi, and edi to survive a callee. */
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi

    /* Let another thread execute guest code while this import runs in host. */
    callq _lp32_guest_execution_leave

    /* dispatch(import_id, guest_arguments, guest_return_address) */
    movl (%r14), %edi
    leaq 8(%r14), %rsi
    movl 4(%r14), %edx
    callq _lp32_dispatch_import

    /* Apply floating/escape state belonging to this host thread. */
    subq $16, %rsp
    movq %rax, (%rsp)
    movq %r14, %rdi
    callq _lp32_adjust_import_stack
    movq %rax, %r14
    callq _lp32_apply_thread_return_state
    movl %eax, 8(%rsp)

    /* A controlled escape returns to run_compat32 without re-entering guest. */
    cmpl $0, 8(%rsp)
    jne 2f
    callq _lp32_guest_execution_enter
2:
    movl 8(%rsp), %ecx
    movq (%rsp), %rax
    addq $16, %rsp
    movq %rax, %rdx
    shrq $32, %rdx

    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx

    cmpl $0, %ecx
    jne 1f

    /* Resume immediately after the original i386 call instruction, via the
       i386 landing trampoline: it verifies the mode and rets to the return
       address, which is left on top of the guest stack (only the import id
       is popped here).  Keep EDX intact: it carries the high half of 64-bit
       i386 returns. */
    movl _lp32_landing32(%rip), %ecx
    movl %ecx, (%rsp)
    movzwl _lp32_cs32(%rip), %ecx
    movl %ecx, 4(%rsp)
    leal 4(%r14), %r14d
    cmpl $0, _lp32_native_segments(%rip)
    je 9f
    movw %ss, %cx
    movw %cx, %ds
    movw %cx, %es
9:
    xchgq %r14, %rsp
    ljmp *(%r14)

1:
    /* Controlled escape: unwind run_compat32's host-side stack frame. */
    cmpl $0, 12(%rsp)
    je 8f
    movw 8(%rsp), %cx
    movw %cx, %ds
    movw 10(%rsp), %cx
    movw %cx, %es
8:
    addq $24, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    retq
