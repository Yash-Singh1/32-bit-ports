.data
.globl _lp32_native_segments
_lp32_native_segments:
    .long 0
.text
.p2align 4, 0x90
.globl _run_compat32
_run_compat32:
    /*
     * uint32_t run_compat32(uint32_t eip, uint32_t esp, uint16_t cs32)
     *
     * r14 is invisible to compatibility-mode code, so it carries the host
     * stack across the 32-bit interval.  The far pointer itself lives on the
     * host stack.  The low-address 64-bit return stub reverses the xchg and
     * returns here with eax untouched.
     */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $24, %rsp
    movl %edi, (%rsp)
    movl %edx, 4(%rsp)
    /* Native Intel checks DS/ES in compatibility mode. A 64-bit thread
       may enter with null data selectors, which Rosetta tolerates. Preserve
       the host selectors in the unused part of the transition frame. */
    movl _lp32_native_segments(%rip), %ecx
    movl %ecx, 12(%rsp)
    testl %ecx, %ecx
    jz 9f
    movw %ds, 8(%rsp)
    movw %es, 10(%rsp)
    movw %ss, %cx
    movw %cx, %ds
    movw %cx, %es
9:
    movl %esi, %r14d
    xchgq %r14, %rsp
    ljmp *(%r14)
    ud2

/* Capture non-EAX guest results before host C code can clobber EDX or ST(0).
   kind: 0 = EDX:EAX, 1 = x87 float, 2 = x87 double. Normal calls retain the
   original run_compat32 path. Used for embedded Cocoa method return values. */
.p2align 4, 0x90
.globl _run_compat32_capture
_run_compat32_capture:
    pushq %rbx
    subq $16, %rsp
    movl %ecx, %ebx
    call _run_compat32
    cmpl $1, %ebx
    je 1f
    cmpl $2, %ebx
    je 2f
    movl %eax, %eax
    shlq $32, %rdx
    orq %rdx, %rax
    jmp 3f
1:  fstps (%rsp)
    movl (%rsp), %eax
    jmp 3f
2:  fstpl (%rsp)
    movq (%rsp), %rax
3:  addq $16, %rsp
    popq %rbx
    ret
