# =========================================
# HISPERIA OS BUILD SYSTEM
# =========================================

PROJECT := HisperiaOS

CC := clang
LD := ld.lld
HOSTCC ?= cc

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso_root

C_SRC := \
    src/main.c \
    src/gdt.c \
    src/idt.c \
    src/isr.c \
    src/pmm.c \
    src/vmm.c \
    src/stack.c \
    src/heap.c \
    src/ring3.c \
    src/blockdev.c \
    src/partition.c \
    src/fat32.c \
    src/ata_pio.c \
    src/pci.c \
    src/ahci.c

ASM_SRC := \
    src/gdt_load.S \
    src/isr.S \
    src/ring3.S \
    src/user_program.S

C_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(C_SRC))
ASM_OBJ := $(patsubst src/%.S,$(BUILD_DIR)/%.asm.o,$(ASM_SRC))
OBJ := $(C_OBJ) $(ASM_OBJ)

KERNEL := $(BUILD_DIR)/kernel.elf
ISO := $(BUILD_DIR)/hisperia.iso

LINKER_SCRIPT := linker.ld
LIMINE_DIR := limine
LIMINE_TOOL := $(LIMINE_DIR)/limine
LIMINE_CONF := limine.conf

# =========================================
# COMPILER FLAGS
# =========================================

RUN_EXCEPTION_TEST ?= 0
RUN_MPROTECT_TEST ?= 0

CFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=gnu11 \
    -O2 \
    -Wall \
    -Wextra \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -fno-pie \
    -fno-builtin \
    -fno-asynchronous-unwind-tables \
    -fno-unwind-tables \
    -mno-red-zone \
    -mcmodel=kernel \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -Iinclude \
    -MMD -MP

CFLAGS += -DHISPERIA_RUN_EXCEPTION_TEST=$(RUN_EXCEPTION_TEST)
CFLAGS += -DHISPERIA_RUN_MPROTECT_TEST=$(RUN_MPROTECT_TEST)

# =========================================
# LINKER FLAGS
# =========================================

LDFLAGS := \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --build-id=none

# =========================================
# DEFAULT
# =========================================

.PHONY: all kernel iso run fat32-disk run-fat32 qemu-fat32 run-sata qemu-sata test-storage-host clean help validate

all: kernel

# =========================================
# BUILD DIRECTORY
# =========================================

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# =========================================
# COMPILE
# =========================================

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.asm.o: src/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Header dependency tracking: ubah include/*.h -> objek terkait dibangun ulang.
-include $(OBJ:.o=.d)


# =========================================
# LINK
# =========================================

$(KERNEL): $(OBJ) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) \
	    -T $(LINKER_SCRIPT) \
	    -o $(KERNEL) \
	    $(OBJ)

kernel: $(KERNEL)
	@echo ""
	@echo "[BUILD] Kernel compiled successfully."
	@echo "[BUILD] $(KERNEL)"
	@echo ""

# =========================================
# ISO GENERATION
# =========================================

iso: $(ISO)

$(ISO): $(KERNEL) $(LIMINE_CONF)
	@test -f $(LIMINE_DIR)/limine-bios.sys || \
	    (echo "[ERROR] Missing limine-bios.sys"; exit 1)

	@test -f $(LIMINE_DIR)/limine-bios-cd.bin || \
	    (echo "[ERROR] Missing limine-bios-cd.bin"; exit 1)

	@test -f $(LIMINE_DIR)/limine-uefi-cd.bin || \
	    (echo "[ERROR] Missing limine-uefi-cd.bin"; exit 1)

	@test -f $(LIMINE_DIR)/BOOTX64.EFI || \
	    (echo "[ERROR] Missing BOOTX64.EFI"; exit 1)

	@test -x $(LIMINE_TOOL) || \
	    (echo "[ERROR] Limine CLI not found or not executable"; exit 1)

	rm -rf $(ISO_ROOT)

	mkdir -p $(ISO_ROOT)/boot/limine
	mkdir -p $(ISO_ROOT)/EFI/BOOT

	cp $(KERNEL) \
	    $(ISO_ROOT)/boot/kernel.elf

	cp $(LIMINE_CONF) \
	    $(ISO_ROOT)/boot/limine/limine.conf

	cp $(LIMINE_DIR)/limine-bios.sys \
	    $(ISO_ROOT)/boot/limine/

	cp $(LIMINE_DIR)/limine-bios-cd.bin \
	    $(ISO_ROOT)/boot/limine/

	cp $(LIMINE_DIR)/limine-uefi-cd.bin \
	    $(ISO_ROOT)/boot/limine/

	cp $(LIMINE_DIR)/BOOTX64.EFI \
	    $(ISO_ROOT)/EFI/BOOT/

	xorriso -as mkisofs \
	    -R -r -J \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot \
	    -boot-load-size 4 \
	    -boot-info-table \
	    -hfsplus \
	    -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part \
	    --efi-boot-image \
	    --protective-msdos-label \
	    $(ISO_ROOT) \
	    -o $(ISO)

	$(LIMINE_TOOL) bios-install $(ISO)

	@echo ""
	@echo "[ISO] Successfully generated!"
	@echo "[ISO] $(ISO)"
	@echo ""

# =========================================
# QEMU
# =========================================

run: iso
	qemu-system-x86_64 \
	    -M q35 \
	    -m 256M \
	    -cdrom $(ISO) \
	    -serial stdio \
	    -monitor none \
	    -no-reboot \
	    -no-shutdown

# 128 MiB MBR-partitioned FAT32 disk image for the legacy IDE PIO test.
FAT32_DISK := $(BUILD_DIR)/fat32-disk.img

fat32-disk: $(FAT32_DISK)

$(FAT32_DISK): tools/create_fat32_image.py | $(BUILD_DIR)
	python3 tools/create_fat32_image.py $@ 128

# Uses i440fx/legacy IDE so the primary-master ATA PIO driver can read the disk.
run-fat32: iso fat32-disk
	qemu-system-x86_64 \
	    -M pc \
	    -m 256M \
	    -cdrom $(ISO) \
	    -drive file=$(FAT32_DISK),format=raw,if=ide,index=0,media=disk \
	    -boot order=d \
	    -serial stdio \
	    -monitor none \
	    -no-reboot \
	    -no-shutdown

qemu-fat32: run-fat32

# AHCI SATA test: q35 + ICH9 AHCI controller + SATA disk on port 0.
run-sata: iso fat32-disk
	qemu-system-x86_64 \
	    -M q35 \
	    -m 256M \
	    -cdrom $(ISO) \
	    -device ich9-ahci,id=ahci \
	    -drive file=$(FAT32_DISK),format=raw,if=none,id=satadisk \
	    -device ide-hd,drive=satadisk,bus=ahci.0 \
	    -boot order=d \
	    -serial stdio \
	    -monitor none \
	    -no-reboot \
	    -no-shutdown

qemu-sata: run-sata

# Pure host-side tests for the sector layer, partition parsers and FAT32 BPB.
HOST_STORAGE_TEST := $(BUILD_DIR)/storage-host-test

test-storage-host: $(HOST_STORAGE_TEST)
	$(HOST_STORAGE_TEST)

$(HOST_STORAGE_TEST): tools/storage_host_test.c src/blockdev.c src/partition.c src/fat32.c | $(BUILD_DIR)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Iinclude \
	    tools/storage_host_test.c src/blockdev.c src/partition.c src/fat32.c \
	    -o $@

# =========================================
# CLEAN
# =========================================

clean:
	rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Build directory removed."

validate:
	./tools/validate.sh

# =========================================
# HELP
# =========================================

help:
	@echo "HisperiaOS Build System"
	@echo ""
	@echo "make        - Build kernel"
	@echo "make kernel - Build kernel"
	@echo "make iso    - Generate bootable ISO"
	@echo "make run    - Run ISO using QEMU (q35, no disk test)"
	@echo "make fat32-disk - Create an MBR + FAT32 test disk image"
	@echo "make run-fat32 - Boot with a FAT32 disk on legacy IDE PIO"
	@echo "make qemu-fat32 - Alias for run-fat32"
	@echo "make test-storage-host - Run host-side block/partition/FAT32 tests"
	@echo "make RUN_EXCEPTION_TEST=1 - Enable deliberate exception test"
	@echo "make RUN_MPROTECT_TEST=1 - Test RO write protection (expected #PF)"
	@echo "make RUN_MPROTECT_TEST=2 - Test NX execution protection (expected #PF)"
	@echo "make validate - Compile normal + both fault-test configurations"
	@echo "Ring 3 demo runs automatically during normal boot"
	@echo "make clean  - Clean build files"
