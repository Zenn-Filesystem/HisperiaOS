/*
 * Tiny position-independent Ring 3 payload.
 * Exercises write/stat/readdir/open/seek/read/close/exit via int 0x80.
 * /etc/hostname is an optional probe on the mounted ext4 root.
 */
.section .text.userblob,"ax",@progbits
.balign 16
.global user_blob_start
.global user_blob_end
.type user_blob_start, @function
user_blob_start:
    /* Stack scratch: stat @ 0, cursor @ 16, dirent @ 32. */
    subq $448, %rsp

    /* write(1, hello_message, hello_length) */
    movq $1, %rax
    leaq hello_message(%rip), %rdi
    movq $(hello_message_end - hello_message), %rsi
    int $0x80

    /* stat("/", &stat_result) -- a mounted root is an optional probe. */
    movq $7, %rax
    leaq user_root_path(%rip), %rdi
    movq %rsp, %rsi
    int $0x80
    testq %rax, %rax
    jnz .Lfilesystem_unavailable
    cmpb $2, 8(%rsp)               /* VFS_TYPE_DIRECTORY */
    jne .Lreaddir_failed

    /* readdir("/", &cursor, &dirent) */
    movl $0, 16(%rsp)
    movq $8, %rax
    leaq user_root_path(%rip), %rdi
    leaq 16(%rsp), %rsi
    leaq 32(%rsp), %rdx
    int $0x80
    cmpq $1, %rax
    jne .Lreaddir_failed

    movq $1, %rax
    leaq readdir_message(%rip), %rdi
    movq $(readdir_message_end - readdir_message), %rsi
    int $0x80

.Lfilesystem_unavailable:
    /* stat("/etc/hostname", &stat_result) -- optional regular-file probe. */
    movq $7, %rax
    leaq user_test_path(%rip), %rdi
    movq %rsp, %rsi
    int $0x80
    testq %rax, %rax
    jnz .Lfile_missing
    cmpb $1, 8(%rsp)               /* VFS_TYPE_REGULAR */
    jne .Lfile_missing

    /* fd = open("/etc/hostname", O_RDONLY) */
    movq $3, %rax
    leaq user_test_path(%rip), %rdi
    xorq %rsi, %rsi
    int $0x80
    cmpq $3, %rax
    jl .Lfile_missing
    movq %rax, %r12

    /* Exercise seek-to-end and seek-to-start. */
    movq %r12, %rdi
    xorq %rsi, %rsi
    movq $2, %rdx                 /* VFS_SEEK_END */
    movq $6, %rax
    int $0x80
    cmpq $0, %rax
    jl .Lclose_then_missing

    movq %r12, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx               /* VFS_SEEK_SET */
    movq $6, %rax
    int $0x80
    cmpq $0, %rax
    jne .Lclose_then_missing

    /* read(fd, user_stack_buffer, 128) */
    movq %r12, %rdi
    leaq 304(%rsp), %rsi
    movq $128, %rdx
    movq $4, %rax
    int $0x80
    testq %rax, %rax
    jle .Lclose_then_missing
    movq %rax, %r13

    /* write(1, bytes_read, bytes_read_count) */
    movq $1, %rax
    leaq 304(%rsp), %rdi
    movq %r13, %rsi
    int $0x80

    /* close(fd) */
    movq %r12, %rdi
    movq $5, %rax
    int $0x80
    jmp .Ldo_exit

.Lclose_then_missing:
    movq %r12, %rdi
    movq $5, %rax
    int $0x80

.Lfile_missing:
    movq $1, %rax
    leaq missing_message(%rip), %rdi
    movq $(missing_message_end - missing_message), %rsi
    int $0x80
    jmp .Ldo_exit

.Lreaddir_failed:
    movq $1, %rax
    leaq readdir_failed_message(%rip), %rdi
    movq $(readdir_failed_message_end - readdir_failed_message), %rsi
    int $0x80
    movq $1, %rdi
    jmp .Ldo_exit_status

.Ldo_exit:
    xorq %rdi, %rdi
.Ldo_exit_status:
    movq $2, %rax
    int $0x80

.Lunexpected_return:
    jmp .Lunexpected_return

hello_message:
    .ascii "[USER] Hello from HisperiaOS Ring 3!\n"
hello_message_end:
readdir_message:
    .ascii "[USER] stat + readdir syscalls PASS\n"
readdir_message_end:
user_root_path:
    .asciz "/"
user_test_path:
    .asciz "/etc/hostname"
missing_message:
    .ascii "[USER] /etc/hostname is unavailable\n"
missing_message_end:
readdir_failed_message:
    .ascii "[USER] readdir syscall FAIL\n"
readdir_failed_message_end:
user_blob_end:
.size user_blob_start, user_blob_end - user_blob_start

.section .note.GNU-stack,"",@progbits
