.section .text

.equ USER_DATA_SELECTOR, 0x2B
.equ USER_CODE_SELECTOR, 0x33

.global ring3_enter
.type ring3_enter, @function
/*
 * void ring3_enter(uint64_t user_rip, uint64_t user_rsp)
 *
 * Saves the SysV callee-saved registers on the original kernel stack. The
 * syscall exit trampoline restores that exact stack and returns to caller.
 */
ring3_enter:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, ring3_saved_kernel_rsp(%rip)

    /* IRETQ frame: SS, RSP, RFLAGS, CS, RIP (pushed in reverse order). */
    pushq $USER_DATA_SELECTOR
    pushq %rsi
    pushq $0x2                    /* Keep IF=0 until hardware IRQ handlers exist. */
    pushq $USER_CODE_SELECTOR
    pushq %rdi
    iretq
.size ring3_enter, .-ring3_enter

.global ring3_exit_trampoline
.type ring3_exit_trampoline, @function
/* Entered by IRETQ at CPL0 after SYS_EXIT rewrites the interrupt frame. */
ring3_exit_trampoline:
    call ring3_finish_exit

    /* Return to the suspended kernel caller with its callee-saved state. */
    movq ring3_saved_kernel_rsp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size ring3_exit_trampoline, .-ring3_exit_trampoline

/*
 * int 0x80 entry. The CPU has already switched to TSS.RSP0 and pushed
 * RIP/CS/RFLAGS/RSP/SS. The saved-register layout is described in ring3.c.
 */
.global isr128
.type isr128, @function
isr128:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    movq %rsp, %r12
    andq $-16, %rsp
    call ring3_syscall_dispatch
    movq %r12, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
.size isr128, .-isr128

.section .bss
.balign 8
.global ring3_saved_kernel_rsp
.type ring3_saved_kernel_rsp, @object
.size ring3_saved_kernel_rsp, 8
ring3_saved_kernel_rsp:
    .zero 8

.section .note.GNU-stack,"",@progbits
