.text
.globl _static_display_bounds, _static_uuid_bytes, _static_gregorian_date, _static_objc_rect
_static_display_bounds:
    jmp _CGDisplayBounds
_static_uuid_bytes:
    jmp _CFUUIDGetUUIDBytes
_static_gregorian_date:
    jmp _CFAbsoluteTimeGetGregorianDate
_static_objc_rect:
    jmp _objc_msgSend_stret

.globl _check_stret_stack
_check_stret_stack:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, %esi
    movl 12(%ebp), %ecx
    movl 16(%ebp), %edi
1:
    testl %ecx, %ecx
    jz 2f
    decl %ecx
    pushl (%edi,%ecx,4)
    jmp 1b
2:
    calll *8(%ebp)
    movl 12(%ebp), %ecx
    shll $2, %ecx
    movl %esi, %edx
    subl %ecx, %edx
    movl 20(%ebp), %ecx
    shll $2, %ecx
    addl %ecx, %edx
    xorl %eax, %eax
    cmpl %edx, %esp
    setne %al
    movl %esi, %esp
    popl %edi
    popl %esi
    popl %ebx
    leave
    ret
