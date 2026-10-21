    .text
    .globl asm_add
    .type asm_add, @function
asm_add:
    leaq (%rdi,%rsi), %rax
    ret
    .size asm_add, .-asm_add
    .section .note.GNU-stack,"",@progbits
