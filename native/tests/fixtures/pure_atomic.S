.text
.globl _start, _atomic_abi, _atomic_add_loop, _atomic_pointer
_start:
    ret
/* Return the loader-bound non-lazy function pointer. */
_atomic_pointer:
    movl Ladd_pointer, %eax
    ret

/* Exercise actual static stubs with preserved registers and FP state. */
_atomic_abi:
    pushl %ebx
    pushl %ebp
    pushl %esi
    pushl %edi
    subl $28, %esp
    movl 48(%esp), %eax
    movl %eax, 20(%esp)
    fnstcw 16(%esp)
    stmxcsr 12(%esp)
    fld1
    movl $0x12345678, %ebx
    movl $0x23456789, %ebp
    movl $0x3456789a, %esi
    movl $0x456789ab, %edi
    movl $1, (%eax)
    movl %eax, 4(%esp)
    movl $-2, (%esp)
    call _OSAtomicAdd32Barrier
    cmpl $-1, %eax
    jne Lfail
    testl %edx, %edx
    jne Lfail
    movl 20(%esp), %ecx
    movl $0x7fffffff, (%ecx)
    movl %ecx, 4(%esp)
    movl $1, (%esp)
    call _OSAtomicAdd32
    cmpl $0x80000000, %eax
    jne Lfail
    testl %edx, %edx
    jne Lfail
    movl 20(%esp), %ecx
    movl $0x80000000, (%esp)
    movl $7, 4(%esp)
    movl %ecx, 8(%esp)
    call _OSAtomicCompareAndSwap32Barrier
    cmpl $1, %eax
    jne Lfail
    testl %edx, %edx
    jne Lfail
    movl $8, (%esp)
    movl $9, 4(%esp)
    call _OSAtomicCompareAndSwapInt
    testl %eax, %eax
    jne Lfail
    testl %edx, %edx
    jne Lfail
    movl 20(%esp), %ecx
    cmpl $7, (%ecx)
    jne Lfail
    cmpl $0x12345678, %ebx
    jne Lfail
    cmpl $0x23456789, %ebp
    jne Lfail
    cmpl $0x3456789a, %esi
    jne Lfail
    cmpl $0x456789ab, %edi
    jne Lfail
    fnstcw 24(%esp)
    movzwl 24(%esp), %eax
    cmpw 16(%esp), %ax
    jne Lfail
    stmxcsr 24(%esp)
    movl 24(%esp), %eax
    cmpl 12(%esp), %eax
    jne Lfail
    movl 20(%esp), %ecx
    fstpl 16(%ecx)
    cmpl $0, 16(%ecx)
    jne Lfail_popped
    cmpl $0x3ff00000, 20(%ecx)
    jne Lfail_popped
    xorl %eax, %eax
    jmp Ldone
Lfail:
    fstp %st(0)
Lfail_popped:
    movl $1, %eax
Ldone:
    addl $28, %esp
    popl %edi
    popl %esi
    popl %ebp
    popl %ebx
    ret

/* Millions of actual imported i386 calls, plus a shared host/guest counter. */
_atomic_add_loop:
    pushl %ebx
    pushl %esi
    subl $20, %esp
    movl 32(%esp), %esi
    movl 36(%esp), %ebx
    testl %ebx, %ebx
    je Lloop_done
Lloop:
    movl $1, (%esp)
    movl %esi, 4(%esp)
    call _OSAtomicAdd32Barrier
    decl %ebx
    jne Lloop
Lloop_done:
    addl $20, %esp
    popl %esi
    popl %ebx
    ret

.section __DATA,__nl_symbol_ptr,non_lazy_symbol_pointers
.p2align 2
Ladd_pointer:
    .indirect_symbol _OSAtomicAdd32
    .long 0

.text
.globl _atomic_consume
/* Host publishes payload i with flag=1. Guest claims 1->2, reads payload,
   then acknowledges 2->0, all with the imported seq_cst CAS. */
_atomic_consume:
    pushl %ebx
    pushl %esi
    pushl %edi
    subl $16, %esp
    movl 32(%esp), %esi
    movl 36(%esp), %edi
    movl $1, %ebx
Lconsume:
    movl $1, (%esp)
    movl $2, 4(%esp)
    movl %esi, 8(%esp)
    call _OSAtomicCompareAndSwap32
    testl %eax, %eax
    je Lconsume
    cmpl %ebx, 4(%esi)
    je Lack
    movl $1, 8(%esi)
Lack:
    movl $2, (%esp)
    movl $0, 4(%esp)
    call _OSAtomicCompareAndSwapPtrBarrier
    testl %eax, %eax
    jne Lack_done
    movl $1, 8(%esi)
Lack_done:
    incl %ebx
    cmpl %edi, %ebx
    jbe Lconsume
    movl 8(%esi), %eax
    addl $16, %esp
    popl %edi
    popl %esi
    popl %ebx
    ret
