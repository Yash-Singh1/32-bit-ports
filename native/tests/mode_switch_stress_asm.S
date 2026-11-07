/* Host side of the mode-switch stress test (see mode_switch_stress.c). */
.text
.p2align 4, 0x90

/* Plain gateway: host call, then ljmp straight to the guest return address
   (the loader's behaviour before the mode guards). */
.globl _stress_gateway_plain
_stress_gateway_plain:
    xchgq %r14, %rsp
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    leaq 8(%r14), %rdi
    callq _stress_host
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    movl 4(%r14), %ecx
    movl %ecx, (%rsp)
    movzwl _stress_cs32(%rip), %ecx
    movl %ecx, 4(%rsp)
    leal 8(%r14), %r14d
    xchgq %r14, %rsp
    ljmp *(%r14)

/* Guarded gateway: ljmp to the i386 landing trampoline, which verifies the
   mode and rets to the guest return address left on the guest stack. */
.globl _stress_gateway_guarded
_stress_gateway_guarded:
    xchgq %r14, %rsp
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    leaq 8(%r14), %rdi
    callq _stress_host
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    movl _stress_landing32(%rip), %ecx
    movl %ecx, (%rsp)
    movzwl _stress_cs32(%rip), %ecx
    movl %ecx, 4(%rsp)
    leal 4(%r14), %r14d
    xchgq %r14, %rsp
    ljmp *(%r14)

.globl _stress_host
_stress_host:
    movl (%rdi), %eax
    movl 4(%rdi), %ecx
    lock xaddl %eax, (%rcx)
    ret
