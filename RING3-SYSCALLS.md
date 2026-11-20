# HisperiaOS Ring 3 syscall ABI (v0.2)

The syscall entry is software interrupt `int 0x80`. The syscall number is passed
in `RAX`; arguments use `RDI`, `RSI`, and `RDX`. Return values are in `RAX`.
A failure returns `-1` unless otherwise noted. User pointers are validated
against the active user address space before the kernel copies data through the
supervisor-only HHDM alias.

| ID | Name | Arguments | Return |
|---:|---|---|---|
| 1 | `write` | `RDI=buffer`, `RSI=count` | bytes written |
| 2 | `exit` | `RDI=status` | does not return to user mode |
| 3 | `open` | `RDI=absolute_path`, `RSI=0` | descriptor (`>=3`) |
| 4 | `read` | `RDI=fd`, `RSI=buffer`, `RDX=count` | bytes read, `0` at EOF |
| 5 | `close` | `RDI=fd` | `0` |
| 6 | `seek` | `RDI=fd`, `RSI=signed_offset`, `RDX=whence` | resulting absolute offset |
| 7 | `stat` | `RDI=absolute_path`, `RSI=struct hisperia_stat *` | `0` |
| 8 | `readdir` | `RDI=absolute_path`, `RSI=uint32_t *cursor`, `RDX=struct hisperia_dirent *` | `1` entry, `0` EOF |

`whence` for `seek` is `0` (start), `1` (current offset), or `2` (end).
The `readdir` cursor is a 32-bit byte offset. Initialize it to zero and pass the
same cursor pointer for subsequent calls. The kernel updates it after each
entry. A `dirent` contains a 32-bit inode, an 8-bit type, three reserved bytes,
and a 256-byte NUL-terminated name field. The `stat` structure contains a
64-bit size, an 8-bit type, and seven reserved bytes.

The current implementation is read-only. `open` supports regular files only;
`readdir` accepts a directory path directly rather than a directory descriptor.
The file descriptor table is a fixed 16-entry table in the single-task Ring 3
demo and will move into per-process state as process management is implemented.
