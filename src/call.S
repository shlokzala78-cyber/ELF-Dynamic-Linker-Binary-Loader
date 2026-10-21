    .text
    .globl lab_call2
    .type lab_call2, @function
lab_call2:
    .cfi_startproc
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    # SysV AMD64 requires 16-byte stack alignment immediately before call.
    subq $8, %rsp
    .cfi_adjust_cfa_offset 8
    call *%rax
    addq $8, %rsp
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
    .size lab_call2, .-lab_call2

    .globl lab_call0
    .type lab_call0, @function
lab_call0:
    .cfi_startproc
    subq $8, %rsp
    .cfi_adjust_cfa_offset 8
    call *%rdi
    addq $8, %rsp
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
    .size lab_call0, .-lab_call0
    .section .note.GNU-stack,"",@progbits
