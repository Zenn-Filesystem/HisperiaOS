# HisperiaOS — OS Architecture & System Overview

> **Dokumen arsitektur dan status pengembangan**  
> Versi dokumentasi: 20 November 2026  
> Platform saat ini: x86-64 (AMD64)  
> Bahasa utama: C dan x86-64 Assembly  
> Bootloader: Limine, base revision 6

---

## 1. Tentang HisperiaOS

**HisperiaOS** adalah sistem operasi x86-64 eksperimental yang dikembangkan secara modular. Proyek ini dimulai dari kernel freestanding dan berkembang menuju sebuah sistem yang dapat melakukan boot, mengenali perangkat penyimpanan, membaca filesystem Linux ext4, menjalankan program userspace ELF64, serta menyediakan fondasi untuk display dan layanan sistem.

Tujuan jangka panjangnya adalah membangun sistem operasi milik sendiri dengan lingkungan aplikasi dan runtime yang dapat dikembangkan secara mandiri. Salah satu arah penggunaan yang direncanakan adalah sistem yang berorientasi pada game dan emulator, tetapi kemampuan tersebut masih merupakan roadmap, bukan fitur yang sudah selesai.

HisperiaOS tidak bertujuan untuk menjadi kernel yang menampung seluruh fitur. Pemisahan tanggung jawab menjadi prinsip utama:

- **Kernel** menangani mekanisme inti: CPU, memori, address space, syscall, dan layanan dasar yang harus memiliki hak istimewa.
- **Driver** menangani komunikasi dengan perangkat keras.
- **Library** menyediakan implementasi yang dapat digunakan ulang dan tidak bergantung pada perangkat tertentu.
- **System** menyediakan program, layanan, runtime, dan pengalaman yang digunakan oleh pengguna maupun aplikasi.

Pada implementasi sekarang, beberapa driver dan filesystem masih dikompilasi ke kernel. Pemindahan driver non-esensial ke system/userspace merupakan arah arsitektur berikutnya, bukan kondisi yang sudah sepenuhnya tercapai.

## 2. Prinsip desain

### 2.1 Kernel kecil, system berkembang di luar kernel

Kernel sebaiknya menyediakan primitive dan API yang stabil. Fitur seperti pengelolaan font, display service, terminal, shell, konfigurasi, dan utilitas sistem tidak seharusnya menjadi bagian dari kernel.

Driver yang benar-benar diperlukan saat boot dapat tetap berada di kernel untuk sementara. Setelah tersedia mekanisme userspace yang memadai—seperti process lifecycle, IPC, memory mapping perangkat, dan pengelolaan hak akses—driver tertentu dapat dipindahkan keluar dari kernel.

### 2.2 Pemisahan perangkat keras dan kebijakan sistem

Driver storage hanya menyediakan akses blok. Parser partisi memahami MBR/GPT. Filesystem memahami struktur ext4. VFS menyediakan antarmuka path/file yang generik. Program userspace menggunakan syscall, bukan mengakses controller storage secara langsung.

Prinsip yang sama digunakan pada input: driver PS/2 menerjemahkan scan code menjadi key event, sedangkan antrean event dan keymap berada di library device-neutral.

### 2.3 Proteksi memori sejak awal

Kernel menggunakan proteksi halaman dan aturan W^X (write XOR execute). Memori yang dapat ditulis tidak boleh sekaligus dapat dieksekusi. Area kernel, stack, heap, dan userspace dipetakan dengan izin yang berbeda sesuai kebutuhannya.

### 2.4 Implementasi bertahap dan dapat diuji

Parser, VFS, keymap, ELF loader, serta beberapa komponen lain memiliki host-side test. Pengujian host membantu memeriksa struktur data dan input yang rusak, tetapi tidak menggantikan pengujian boot dan hardware sesungguhnya.

## 3. Gambaran arsitektur

```text
                         Firmware / UEFI / BIOS
                                  |
                                  v
                         Limine Bootloader
                           (base revision 6)
                                  |
                                  v
+----------------------------------------------------------------+
|                         HISPERIA KERNEL                         |
|                                                                |
|  CPU: GDT / TSS / IDT / ISR / exceptions                        |
|  Memory: PMM / VMM / heap / address spaces                      |
|  Kernel services: block device / VFS / syscall / ELF launcher   |
|  Early display: framebuffer console + embedded bitmap font      |
|                                                                |
|  Drivers (current in-kernel implementations):                  |
|    PCI | AHCI | ATA PIO | PS/2 keyboard | ACPI power            |
+----------------------------------------------------------------+
                |                              |
                v                              v
        Storage abstraction              Ring 3 / int 0x80
                |                              |
        MBR / GPT parser                       VFS syscalls
                |                              |
          ext4 read-only                       ELF64 loader
                |                              |
          Generic VFS                          /system/bin/*.elf
                |                              |
                +--------------+---------------+
                               |
                               v
                    Future userspace system
                    - init / service manager
                    - shell and core utilities
                    - display service
                    - font library / TTF catalog
                    - application runtime
                    - future user-space drivers
```

The diagram combines components that exist in the current kernel source with system-side components developed separately. It does not imply that a complete userspace service manager, shell, or display server is already running.

## 4. Boot sequence

The current boot path is approximately:

1. **Limine handoff** — the kernel receives boot information, memory map, HHDM information, framebuffer information, and ACPI RSDP information where available.
2. **Early output** — framebuffer console and COM1 serial diagnostics are initialized. The early console uses an embedded bitmap font.
3. **CPU tables** — GDT, TSS, IDT, interrupt/exception stubs, and the double-fault IST stack are installed and checked.
4. **Physical memory** — PMM initializes from the Limine memory map and provides 4 KiB frame allocation.
5. **Virtual memory and protection** — VMM inspects the active four-level page tables, enables CR0.WP and EFER.NXE where supported, applies kernel section permissions, and protects HHDM aliases.
6. **Kernel stack and heap** — a guarded IST1 stack and page-backed kernel heap are prepared and tested.
7. **Address-space self-test** — separate user address spaces are created, switched, inspected, and destroyed.
8. **Storage discovery** — AHCI is probed first. If the supported ext4 root cannot be mounted through it, the kernel tries legacy primary-master ATA PIO.
9. **Partition and filesystem discovery** — MBR/GPT partitions are scanned; a supported ext4 partition is mounted read-only as the VFS root `/`.
10. **Built-in Ring 3 demonstration** — the kernel enters CPL3 with a small built-in payload and exercises the syscall interface.
11. **Early input and power setup** — PS/2 keyboard and ACPI power-button support are initialized when available.
12. **ELF launch** — if a VFS root is mounted, the kernel searches `/system/bin/` for an ELF program. `init.elf` has priority; otherwise the first regular `.elf` entry is selected.
13. **Early polling loop** — after the launched program exits, the current kernel returns to its polling input loop.

The ELF launcher is a temporary single-task boot mechanism. It is not yet a general process manager or an `init`/service framework.

## 5. Kernel core

### 5.1 CPU and exception handling

Current CPU-related components include:

- GDT with kernel and user code/data descriptors.
- TSS with a Ring 0 stack pointer for privilege transitions.
- IDT and exception reporting.
- IST1 stack for double-fault handling, with a guard page below the mapped stack.
- DPL 3 interrupt gate at vector `0x80` for system calls.

The selector values currently used are kernel code `0x08`, kernel data `0x10`, TSS `0x18`, user data `0x2B`, and user code `0x33`.

### 5.2 Physical memory manager (PMM)

The PMM uses a bitmap allocator for 4 KiB physical frames and supports allocation/free validation. The kernel uses Limine's HHDM to access physical memory through a higher-half direct map.

### 5.3 Virtual memory manager (VMM)

The VMM currently provides:

- Four-level x86-64 page-table inspection and mapping traversal.
- 1 GiB and 2 MiB huge-page splitting when a smaller mapping is required.
- Supervisor mapping and unmapping helpers.
- Independent user address-space roots.
- User mapping inspection and destruction of private lower-half page-table trees.
- Shared supervisor-only kernel upper-half mappings.
- CR3 switching between the kernel and a user address space.

The implementation is currently single-CPU oriented. SMP synchronization and TLB shootdown are not implemented.

### 5.4 Memory protection

The kernel enables CR0.WP and EFER.NXE and applies section-level permissions:

| Region | Current permission |
|---|---|
| Kernel `.text` | Read + Execute, supervisor |
| Kernel `.rodata` | Read-only, NX, supervisor |
| Kernel `.data` / `.bss` | Read + Write, NX, supervisor |
| Heap | Read + Write, NX, supervisor |
| IST1 stack | Read + Write, NX, supervisor |
| User mappings | User-accessible, NX by default; W^X enforced |
| Guard page | Not present |

HHDM aliases for the kernel sections are adjusted as well, so the direct map does not trivially bypass the intended section permissions.

### 5.5 Kernel heap

A page-backed heap exposes `kmalloc()` and `kfree()`. The allocator supports block splitting and adjacent free-block coalescing. It is validated by allocation, data-integrity, free, and coalescing tests.

## 6. Storage architecture

Storage is divided into a hardware transport, a common block-device interface, partition parsing, filesystem implementation, and VFS dispatch.

```text
AHCI SATA driver ---------+
                          +--> block_device --> MBR/GPT --> ext4 --> VFS
ATA PIO driver -----------+
                          |
RAM-disk test backend ----+
```

### 6.1 Block-device interface

The common block-device abstraction exposes 512-byte sector reads. It is used by AHCI, legacy ATA PIO, and a RAM-disk backend for tests. Higher layers do not need to know which controller supplied the sectors.

### 6.2 AHCI SATA

The AHCI implementation is an experimental polling driver. It discovers AHCI controllers through PCI configuration space, locates BAR5/ABAR, detects SATA ATA devices, and uses DMA commands including READ DMA EXT. The current implementation is read-only and intentionally narrow: it uses a limited command/PRDT arrangement and does not implement NCQ, hot-plug, ATAPI, or interrupt-driven completion.

Development testing has reported successful SATA device detection and partition enumeration on the target physical laptop. This is not a claim of compatibility with every AHCI controller.

### 6.3 Legacy ATA PIO

A separate primary-master ATA PIO implementation provides a fallback transport. It is polling-based, read-only, and currently limited to LBA28. The QEMU legacy IDE test path exercises this backend.

### 6.4 Partition parsing

The partition layer supports:

- MBR partition tables.
- GPT partition tables.
- GPT CRC32 validation.
- Partition start LBA, sector count, and type identification.

### 6.5 FAT32 status

FAT32 BPB and geometry validation exist, and the storage test image can be detected. FAT32 is **not yet registered as a generic VFS backend**, so it is not currently usable by the Ring 3 file syscalls as a mounted filesystem.

### 6.6 ext4 read-only backend

The ext4 implementation is connected to the generic VFS and supports a deliberately limited read-only profile. Its supported features include common block sizes, supported inode/group descriptor geometries, extent-based files and directories, and selected ext4 incompat/ro-compat features.

The implementation includes CRC32C metadata checksum handling for metadata it reads, including:

- Superblock checksum and checksum-type validation.
- `CSUM_SEED` or UUID-derived checksum seed.
- Group descriptor checksums.
- Inode checksums.
- External extent-tree checksums.
- Directory leaf and HTree block checksums.

Indexed directories are currently scanned linearly rather than using HTree hash traversal. The driver rejects unsupported feature combinations rather than trying to interpret them.

Important limitations:

- Read-only: no create, write, truncate, unlink, rename, or allocation.
- No journal replay. Volumes requiring recovery are rejected.
- Not every ext4 metadata structure is checksummed by this reader; only metadata used by the implemented read path is covered.
- No symlink following, xattrs, encryption, inline data, compression, bigalloc, or complete ext4 feature coverage.
- Only one VFS root is mounted at a time.

Use a cleanly unmounted ext4 volume for development tests. Do not treat this experimental reader as a production filesystem implementation.

## 7. Generic VFS

The VFS separates path-based file operations from filesystem-specific inode and block handling. A filesystem registers an operations table; VFS selects the mounted filesystem and dispatches operations through that table.

Current public operations include:

- `vfs_mount()` and `vfs_mount_ext4()`.
- `vfs_open()` / `vfs_close()`.
- `vfs_read()`.
- `vfs_seek()`.
- `vfs_stat()`.
- `vfs_readdir()`.

The current implementation mounts one filesystem as `/`. Paths are absolute. The ext4 backend currently provides regular-file reads and directory enumeration. There is no generic write API and no mount namespace.

## 8. Userspace and system calls

### 8.1 Ring 3 execution

HisperiaOS has a working kernel-side foundation for entering CPL3, using a separate lower-half address space and a user stack. A built-in assembly payload demonstrates the transition and syscall round-trip.

The user address space has private user mappings, while the kernel half is shared as supervisor-only. On syscall exit, a trampoline restores the kernel CR3, destroys the temporary address space, and resumes the kernel caller.

### 8.2 Syscall ABI

The current syscall entry is `int 0x80`.

- Syscall number: `RAX`
- First argument: `RDI`
- Second argument: `RSI`
- Third argument: `RDX`
- Return value: `RAX`

| ID | Name | Purpose |
|---:|---|---|
| 1 | `write` | Write bytes to the kernel console |
| 2 | `exit` | Exit the current synchronous Ring 3 task |
| 3 | `open` | Open a regular VFS file read-only |
| 4 | `read` | Read bytes from an open descriptor |
| 5 | `close` | Close a descriptor |
| 6 | `seek` | Change a file offset |
| 7 | `stat` | Query file metadata |
| 8 | `readdir` | Enumerate entries in an absolute directory path |

The kernel validates userspace pointers against the active address space and copies data through a supervisor HHDM alias rather than directly trusting user pointers.

Current syscall limitations:

- Filesystem access is read-only.
- `open` currently supports regular files only.
- `readdir` receives an absolute path and a 32-bit byte-offset cursor.
- The descriptor table is a fixed 16-entry table shared by the current test/launcher code, not a per-process resource table.
- The ABI is early and may evolve before it is treated as stable for third-party applications.

## 9. ELF64 program loader

The current ELF loader reads an executable through VFS and maps it into a new user address space.

### 9.1 Supported executable profile

- ELF64, little-endian.
- Machine: x86-64 (`EM_X86_64`).
- Type: static `ET_EXEC`.
- Up to 16 `PT_LOAD` segments.
- Maximum file size: 16 MiB.
- Maximum aggregate load memory: 64 MiB.
- BSS is zero-filled from `p_memsz - p_filesz`.
- Eight writable, NX user stack pages.
- One unmapped guard page below the user stack.
- Initial stack contains a minimal empty argument/environment setup and auxiliary entries such as `AT_PAGESZ`, `AT_ENTRY`, and `AT_NULL`.

The loader checks ELF identity, header and program-header bounds, segment file/memory bounds, arithmetic overflow, alignment constraints, entry-point placement, and overlap between load segments. PT_LOAD segments that share a virtual page are rejected in this initial implementation. Segment permissions are mapped as user RX, user read-only/NX, or user RW/NX. RWX segments are rejected.

### 9.2 Boot-time selection

The kernel scans `/system/bin/` for regular files with a `.elf` suffix. `/system/bin/init.elf` takes priority. If it is absent, the first matching entry returned by VFS is selected.

The program must use the HisperiaOS syscall ABI and terminate with syscall `HISPERIA_SYS_EXIT` (number 2). The launcher is synchronous: there is no scheduler, and a program that loops forever will keep the CPU. Unexpected user exceptions still follow the kernel fatal exception path.

### 9.3 Not supported

- `ET_DYN` / PIE.
- Relocations and dynamic linking.
- `PT_INTERP`, `PT_DYNAMIC`, shared objects, and TLS.
- Demand paging.
- Caller-supplied arguments and environment variables.
- Ordinary Linux executables or Linux ABI compatibility.

This loader is an early application-loading milestone, not yet a general process subsystem.

## 10. Input subsystem

The initial keyboard driver targets a legacy i8042/PS/2 keyboard port. It polls controller status/data ports, enables Set-2-to-Set-1 translation, and turns scan-code make/break sequences into normalized HID-style key events.

Input handling is split into layers:

```text
i8042 / PS/2 hardware
        |
        v
drivers/input/ps2_keyboard.c
        |
        v
lib/input/keyboard.c   --> device-neutral key event queue
        |
        v
lib/input/keymap_us.c --> US keymap / ASCII translation
        |
        v
early console echo
```

The current implementation covers basic alphanumeric keys, US punctuation, Space, Enter, Backspace, Tab, Escape, modifier keys, Caps Lock, F1–F12, and common extended navigation key events.

Limitations include polling-only operation, no IRQ1/PIC/APIC routing, no USB HID, no Bluetooth keyboard, no mouse, no alternate layouts, no terminal line discipline, and no command history. Hardware runtime behavior still needs to be checked on supported target hardware or QEMU.

## 11. ACPI power management

The experimental ACPI component:

- Receives the RSDP address through Limine.
- Finds RSDT/XSDT and FADT.
- Validates ACPI table checksums.
- Reads legacy PM1a event/control I/O blocks.
- Attempts to enable ACPI mode where required.
- Parses the common DSDT `_S5` package form.
- Enables and polls the fixed power-button event.
- Requests ACPI S5 soft-off when the event is detected.

This is a polling prototype, not a complete ACPI implementation. It does not support Hardware-Reduced ACPI or Extended GAS-only layouts, and `_S5` parsing is not a general AML interpreter. SCI/IOAPIC interrupt routing is not implemented. Actual power-off behavior requires runtime verification on the target machine.

## 12. Display and font system

### 12.1 Current kernel display

The kernel receives a framebuffer from Limine and uses it for early boot diagnostics and console output. The current early console uses an embedded bitmap font. The framebuffer is still private to the kernel's early console; a stable userspace framebuffer/device interface has not yet been implemented.

### 12.2 System-side TrueType library

A separate system-side TTF/font component has been developed under:

```text
/system/display/
├── include/
│   └── font.h
├── font/
│   ├── ttf.c
│   └── catalog.c
└── fonts/
    └── *.ttf
```

The intended font directory is:

```text
/system/display/fonts/*.ttf
```

The font library is deliberately outside the kernel object list. It accepts caller-provided file, directory, and allocation callbacks, so it does not depend directly on kernel headers or hardware drivers.

The current TTF implementation supports TrueType `glyf` outlines, cmap formats 4 and 12, horizontal metrics, simple glyphs, XY-positioned composite glyphs, UTF-8 text, integer fixed-point curve processing, grayscale coverage, 4x4 supersampling, clipping, and alpha blending into a caller-owned 32-bit RGB memory surface.

The existing Ring 3 syscall set has the file operations needed to back the font library callbacks: `open`, `read`, `seek`, `close`, `stat`, and `readdir`. The library can therefore be connected to system runtime I/O without embedding a font parser in the kernel.

Not supported yet: OpenType CFF outlines, variable fonts, hinting, kerning, shaping, colored glyphs, bidirectional/right-to-left layout, and composite glyph point-number attachment.

**Important integration status:** the TTF engine has passed standalone host tests and freestanding library build tests in its separate system-side package. It is not yet part of the kernel build, and TTF-rendered pixels cannot yet be presented on the physical display because the userspace framebuffer/display-service interface is still missing.

## 13. Source organization

The kernel source package currently uses this layout:

```text
src/
  main.c                 boot flow and early console/input loop
  runtime.c              minimal freestanding runtime helpers
  elf64.c                ELF64 validation and loading
  ring3.c, ring3.S       userspace transition and syscall path
  vfs.c                  generic VFS dispatcher
  blockdev.c             block-device core and test backend
  pmm.c, vmm.c           physical and virtual memory management
  heap.c, stack.c        kernel heap and stack support
  gdt.c, idt.c, isr.*    CPU tables and exception/interrupt entry

drivers/
  bus/pci.c               PCI enumeration
  firmware/acpi_power.c  ACPI power-button/S5 prototype
  input/ps2_keyboard.c   PS/2 input transport
  storage/ahci.c          AHCI SATA transport
  storage/ata_pio.c       legacy ATA PIO transport

lib/
  fs/ext4.c               ext4 read-only backend
  fs/fat32.c              FAT32 BPB/geometry validation
  input/keyboard.c        generic key event queue
  input/keymap_us.c       US keymap translation
  storage/partition.c     MBR/GPT parsing

include/                  public subsystem interfaces
limine/                   bootloader assets
 tools/                   host tests and image/program helpers
```

The system-side display/font component is maintained separately under `system/display/` in the TTF milestone package. It should remain outside the kernel link.

## 14. Build and development

### 14.1 Kernel source package

Build requirements include Clang, LLD, and the included Limine assets. ISO generation additionally requires `xorriso`; QEMU execution requires `qemu-system-x86_64`.

```sh
make clean
make validate
make kernel
make demo-elf
make iso
make run
```

Useful storage test targets:

```sh
make test-storage-host
make test-vfs-host
make test-ext4-host
make test-keyboard-host
make test-elf64-host
make run-fat32
make run-sata
```

`make demo-elf` builds a small static HisperiaOS ABI test executable at `build/demo.elf`. Copy it into the ext4 root as `/system/bin/demo.elf` before booting HisperiaOS. Since the current ext4 backend is read-only, the file must be copied while the filesystem is mounted by the host OS, then safely unmounted before testing.

### 14.2 System font library package

In the separate TTF/display package:

```sh
make system-font
make test-font-host
```

The font host test can be directed to a specific installed font with `FONT_TEST_FILE=/path/to/font.ttf` and `FONT_TEST_DIR=/path/to/fonts`.

### 14.3 Validation meaning

Host tests validate parser behavior, VFS dispatch, metadata checksums, keymap/event-queue logic, ELF validation and mapping through a mock VMM, and TTF parsing/rasterization into a memory surface. Kernel compile/link and memory-protection test variants validate that the freestanding kernel builds with the intended configurations.

These tests do **not** prove that every device driver works on all hardware, that the new ELF loader completes a real CPL3 run on the target machine, or that TTF text is displayed on the physical framebuffer.

## 15. Current status summary

| Subsystem | Status | Notes |
|---|---|---|
| Limine boot / x86-64 kernel | Implemented | Base revision 6 |
| GDT / TSS / IDT / exceptions | Implemented | IST1 guarded stack |
| PMM / VMM / heap | Implemented | Host/build self-tests; single-CPU oriented |
| W^X / NX memory protection | Implemented | Includes section/HHDM permission checks |
| Separate user address spaces | Implemented foundation | Not a full process subsystem |
| Ring 3 / `int 0x80` | Implemented foundation | Synchronous demonstration and syscall ABI |
| AHCI SATA | Experimental | Physical device/partition detection has been reported |
| ATA PIO | Experimental | Primary-master, polling, read-only fallback |
| MBR / GPT | Implemented | GPT CRC validation |
| FAT32 | Partial | BPB/geometry detection; no VFS mount |
| ext4 | Partial, read-only | Selected feature profile with CRC32C checks for supported metadata |
| Generic VFS | Implemented foundation | One root mount, limited operations |
| ELF64 loader | Implemented prototype | Static ET_EXEC only; runtime test still required |
| PS/2 keyboard | Experimental | Polling, US keymap; target runtime verification needed |
| ACPI power button | Experimental | Legacy PM1 polling and S5 request |
| Early framebuffer console | Implemented | Embedded bitmap font |
| TTF font library | System-side prototype | Standalone tests; not yet presented to physical framebuffer |
| Shell / service manager / libc | Not implemented | Future system work |
| Scheduler / multitasking | Not implemented | Current launcher is synchronous |
| USB / NVMe / networking | Not implemented | Future driver work |

## 16. Known architectural limitations

The project is still an experimental OS foundation. The most important missing pieces are:

- Scheduler, multitasking, and per-thread kernel stacks.
- A general process model with per-process file descriptors and address-space lifecycle.
- User exception handling and clean process termination on faults.
- Interrupt-controller setup and interrupt-driven device operation.
- A stable, documented userspace ABI and runtime library/libc.
- IPC and service management for system services and future userspace drivers.
- Writable filesystem support, journal handling, and broader filesystem compatibility.
- FAT32 VFS integration and additional storage transports such as NVMe and USB mass storage.
- Userspace framebuffer/device API and display service.
- TTF font integration into a working userspace display stack.
- Shell, core utilities, configuration tools, and application runtime.
- SMP synchronization, TLB shootdown, and five-level paging support.

## 17. Suggested development direction

A practical next sequence is:

1. Stabilize the current ELF64 boot launcher and the syscall ABI with QEMU and physical-hardware runtime tests.
2. Introduce a proper process object: address space, saved CPU context, descriptor table, exit status, and per-process kernel stack.
3. Add a scheduler and interrupt/timer foundations so one userspace program cannot monopolize the CPU indefinitely.
4. Build a minimal freestanding userspace runtime and a first `/system/init.elf`.
5. Add a terminal and shell with basic utilities using the existing VFS syscalls.
6. Add IPC and device/service interfaces before moving suitable drivers from the kernel into userspace.
7. Expose a controlled framebuffer interface and create a display service.
8. Connect the system-side TTF library to the display service and load fonts from `/system/display/fonts/`.
9. Expand filesystem and hardware support incrementally, without sacrificing the kernel/system boundary.

---

## 18. Project statement

HisperiaOS is a modular x86-64 operating-system project focused on building its own kernel foundation and gradually moving toward an independent system environment. Its current foundation includes protected memory management, isolated userspace address spaces, a read-only ext4/VFS storage path, a small syscall ABI, and a prototype static ELF64 loader. Input, ACPI power handling, and system-side TrueType rendering are experimental components at different integration stages.

The central architectural direction is to keep the kernel responsible for essential mechanisms while moving higher-level functionality—display, fonts, shell, utilities, services, and eventually suitable drivers—into the system itself.
