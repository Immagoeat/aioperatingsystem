# auroraOS

A tiny 32-bit x86 hobby operating system kernel, written from scratch in C and
x86 assembly. It boots via GRUB (Multiboot) in QEMU (or on real hardware /
other BIOS VMs) and drops you into an interactive shell.

## Features

- Multiboot-compliant kernel entry (boots via GRUB)
- Global Descriptor Table (GDT) setup
- Interrupt Descriptor Table (IDT) with ISR/IRQ stubs for CPU exceptions and
  hardware interrupts
- 8259 PIC remapping
- Programmable Interval Timer (PIT) driver, 100 Hz tick
- PS/2 keyboard driver (scancode set 1, shift/caps lock handling)
- VGA text-mode console driver (80x25, 16 colors, scrolling, cursor)
- Minimal freestanding libc (`string.c`, a tiny `printf`)
- Multiboot memory map parsing
- An interactive shell with builtin commands:
  `help`, `about`, `clear`, `echo`, `mem`, `uptime`, `color`, `reboot`, `halt`

## Requirements

- `gcc` with 32-bit support (`-m32`)
- GNU `ld`, `binutils`
- `grub-mkimage` (from `grub-pc-bin` / `grub2-common`)
- `genisoimage` or `mkisofs` (for ISO packaging — no `xorriso` required)
- `qemu-system-i386` (to run it)

## Building

```sh
make kernel      # build kernel_bin/auroraos.bin only
make iso         # build kernel + auroraos.iso (bootable CD image)
```

The ISO is built by [scripts/make_iso.sh](scripts/make_iso.sh), which uses
`grub-mkimage` directly (targeting `i386-pc-eltorito`) with the GRUB config
embedded via a memdisk, then packages it into a bootable El Torito ISO with
`genisoimage`. This avoids depending on `grub-mkrescue`/`xorriso`, which may
not be installed everywhere.

## Running

```sh
make run         # boot the raw kernel binary directly in QEMU (fastest)
make run-iso     # boot the full ISO through GRUB (closer to real hardware)
```

Once booted, you'll land in the `aurora:~$` shell prompt. Type `help` to see
available commands.

## Project layout

- [kernel/boot.s](kernel/boot.s) — Multiboot header, entry point, GDT/IDT
  flush, ISR/IRQ assembly stubs
- [kernel/kernel.c](kernel/kernel.c) — kernel entry point, boot sequence
- [kernel/gdt.c](kernel/gdt.c), [kernel/idt.c](kernel/idt.c),
  [kernel/pic.c](kernel/pic.c) — CPU/interrupt setup
- [kernel/timer.c](kernel/timer.c), [kernel/keyboard.c](kernel/keyboard.c) —
  drivers
- [kernel/vga.c](kernel/vga.c) — text console
- [kernel/shell.c](kernel/shell.c) — interactive shell
- [kernel/memory.c](kernel/memory.c) — multiboot memory map
- [kernel/linker.ld](kernel/linker.ld) — link script (loads at 1 MiB)
- [scripts/make_iso.sh](scripts/make_iso.sh) — bootable ISO builder

## What this is (and isn't)

This is a real, from-scratch kernel: it manages its own interrupts, drivers,
and console — nothing here rides on Linux or any other existing OS. It's a
single-tasking, single-address-space 32-bit kernel meant as a hobby-OS
starting point, not a production or multi-user operating system. There's no
paging/virtual memory, no filesystem, no process scheduler, and no userspace
program loader yet — natural next steps if you want to keep extending it.
