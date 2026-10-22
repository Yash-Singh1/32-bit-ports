/* Synthetic CPU-only fixture. No original game code or assets. */
.text
.globl _start, _a6_float_value
_start:
    ret
_a6_float_value:
    movl %esp, %eax
    andl $15, %eax
    cmpl $12, %eax
    jne 1f
    flds 4(%esp)
    ret
1:
    fldz
    ret
