.section .text
.global gdt_load
.type gdt_load, @function

gdt_load:
    /*
     * RDI = alamat GDTR
     */
    lgdt (%rdi)

    /*
     * Reload CS menggunakan selector 0x08
     */
    pushq $0x08

    leaq .Lreload_cs(%rip), %rax
    pushq %rax

    lretq

.Lreload_cs:

    /*
     * Reload data segment registers
     */
    movw $0x10, %ax

    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    /*
     * Load Task Register.
     *
     * Selector 0x18 = TSS descriptor.
     */
    movw $0x18, %ax
    ltr %ax

    ret

.size gdt_load, .-gdt_load

.section .note.GNU-stack,"",@progbits