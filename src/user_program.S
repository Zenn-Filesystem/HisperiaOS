/*
 * Tiny position-independent Ring 3 payload.
 * The complete blob (instructions + message) is copied verbatim into one
 * user RX page. All RIP-relative references remain valid after the copy.
 */
.section .text.userblob,"ax",@progbits
.balign 16
.global user_blob_start
.global user_blob_end
.type user_blob_start, @function
user_blob_start:
    /* write(1, message, message_length) */
    movq $1, %rax
    leaq user_message(%rip), %rdi
    movq $(user_message_end - user_message), %rsi
    int $0x80

    /* Treat a short write as failure, then exit(status). */
    cmpq $(user_message_end - user_message), %rax
    jne .Lwrite_failed
    xorq %rdi, %rdi
    jmp .Ldo_exit

.Lwrite_failed:
    movq $1, %rdi

.Ldo_exit:
    movq $2, %rax
    int $0x80

.Lunexpected_return:
    jmp .Lunexpected_return

user_message:
    .ascii "[USER] Hello from HisperiaOS Ring 3!\n"
user_message_end:
user_blob_end:
.size user_blob_start, user_blob_end - user_blob_start

.section .note.GNU-stack,"",@progbits
