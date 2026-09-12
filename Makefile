CC = gcc
AS = gcc
LD = ld

CFLAGS = -m32 -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pie -no-pie
ASFLAGS = -m32 -ffreestanding -c
LDFLAGS = -m elf_i386 -T kernel/linker.ld -nostdlib

KERNEL_SRCS_C = $(wildcard kernel/*.c)
KERNEL_OBJS_C = $(KERNEL_SRCS_C:.c=.o)
KERNEL_OBJ_S = kernel/boot.o

ISO = auroraos.iso
KERNEL_BIN = kernel_bin/auroraos.bin

.PHONY: all kernel iso run run-iso clean

all: iso

kernel/boot.o: kernel/boot.s
	$(AS) $(ASFLAGS) $< -o $@

kernel/%.o: kernel/%.c kernel/kernel.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel: $(KERNEL_OBJ_S) $(KERNEL_OBJS_C)
	mkdir -p kernel_bin
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(KERNEL_OBJ_S) $(KERNEL_OBJS_C)

# Builds auroraos.iso via scripts/make_iso.sh (grub-mkimage + genisoimage),
# which works even when xorriso/grub-mkrescue are unavailable.
iso: kernel
	./scripts/make_iso.sh

# Boot straight into the raw multiboot kernel binary (fastest, no ISO/GRUB).
run: kernel
	qemu-system-i386 -kernel $(KERNEL_BIN)

# Boot the full ISO through GRUB, closest to how real hardware would see it.
run-iso: iso
	qemu-system-i386 -cdrom $(ISO)

clean:
	rm -f kernel/*.o
	rm -rf kernel_bin build
	rm -f $(ISO)
