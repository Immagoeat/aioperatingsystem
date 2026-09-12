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
- PS/2 mouse driver (IRQ12, standard 3-byte packet protocol)
- VGA text-mode console driver (80x25, 16 colors, scrolling, cursor)
- VGA mode 13h graphics driver (320x200, 256 colors) with a small graphics
  library: pixels, lines, rects, an 8x8 bitmap font, and a back buffer
- A tiny windowing GUI: draggable/closable windows with title bars and drop
  shadows, a desktop background, a taskbar, a mouse cursor, and a few demo
  apps (About, an animated uptime counter, a palette swatch viewer)
- Minimal freestanding libc (`string.c`, a tiny `printf`)
- Multiboot memory map parsing
- An interactive shell with builtin commands:
  `help`, `about`, `clear`, `echo`, `mem`, `uptime`, `color`, `gui`,
  `reboot`, `halt`

## The GUI

Type `gui` at the shell prompt to switch into the graphical desktop. Move
the mouse to control the cursor, drag windows by their title bars, click a
window (or its taskbar button) to bring it to the front, and click the `x`
in a title bar to close it. Press `q` on the keyboard to exit back to the
text shell at any time.

Switching between text mode and VGA mode 13h and back is done by hand
(programming the CRTC/Sequencer/Graphics-Controller/Attribute-Controller
registers directly — there's no BIOS to call once we're in 32-bit protected
mode). The trickiest part was that mode 13h uses "chain-4" addressing,
which scatters whatever gets drawn across all four VGA memory planes; text
mode reads plane 0 for character codes and plane 1 for attributes, so
switching back without clearing every plane first shows up as scrambled
"static" instead of clean text. [kernel/vgamode13.c](kernel/vgamode13.c)
clears all four planes and reloads the font into plane 2 before restoring
the original (snapshotted-at-boot) text-mode registers.

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
- [kernel/timer.c](kernel/timer.c), [kernel/keyboard.c](kernel/keyboard.c),
  [kernel/mouse.c](kernel/mouse.c) — drivers
- [kernel/vga.c](kernel/vga.c) — text console
- [kernel/vgamode13.c](kernel/vgamode13.c) — VGA mode 13h graphics mode
  switch, text-mode snapshot/restore, palette control
- [kernel/font8x8.c](kernel/font8x8.c) — 8x8 bitmap font (ASCII 0x20-0x7E)
- [kernel/gfx.c](kernel/gfx.c) — graphics primitives + back buffer
- [kernel/wm.c](kernel/wm.c) — window manager / compositor and demo apps
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
