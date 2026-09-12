# auroraOS

A tiny 32-bit x86 hobby operating system kernel, written from scratch in C and
x86 assembly. It boots via GRUB (Multiboot) into a high-resolution true-color
graphics mode and drops you into a software console, with a modern windowed
desktop GUI a `gui` command away.

## Features

- Multiboot-compliant kernel entry, boots via GRUB
- GRUB-negotiated VBE graphics mode (1024x768x32 requested via the Multiboot
  video-mode header fields; GRUB performs the actual BIOS video call before
  handing off to the kernel) — see [kernel/boot.s](kernel/boot.s)
- Global Descriptor Table (GDT) setup
- Interrupt Descriptor Table (IDT) with ISR/IRQ stubs for CPU exceptions and
  hardware interrupts
- 8259 PIC remapping
- Programmable Interval Timer (PIT) driver, 100 Hz tick
- PS/2 keyboard driver (scancode set 1, shift/caps lock handling)
- PS/2 mouse driver (IRQ12, standard 3-byte packet protocol)
- A true-color linear-framebuffer graphics library: pixels, lines, rects,
  rounded rects, alpha blending, soft drop shadows, an 8x8 bitmap font
  (scalable), and a back buffer — see [kernel/gfx.c](kernel/gfx.c)
- A software text console rendered on the graphics framebuffer (there is no
  legacy VGA text mode in this design — see [kernel/console.c](kernel/console.c))
- A modern-flat-design windowing GUI: soft shadows, rounded corners,
  macOS-style traffic-light window controls, a gradient desktop background,
  a taskbar with a live clock, and a few demo apps (About, an animated
  uptime counter, a color palette viewer) — see [kernel/wm.c](kernel/wm.c)
- Minimal freestanding libc (`string.c`, a tiny `printf`)
- Multiboot memory map + framebuffer info parsing
- An interactive shell with builtin commands:
  `help`, `about`, `clear`, `echo`, `mem`, `uptime`, `gui`, `reboot`, `halt`

## The GUI

Type `gui` at the shell prompt to switch into the graphical desktop. Move
the mouse to control the cursor, drag windows by their title bars, click a
window (or its taskbar button) to bring it to the front, and click the red
button in a title bar to close it. Press `q` on the keyboard to return to
the text shell at any time.

Unlike the classic "hobby OS" approach of banging VGA CRTC/Sequencer/GC
registers directly to switch video modes, auroraOS never touches legacy VGA
mode registers at all: GRUB sets up a single VBE linear-framebuffer mode
once at boot (requested via the Multiboot header in
[kernel/boot.s](kernel/boot.s)), and both the text console and the windowed
GUI are just two different things drawn onto that same framebuffer with
[kernel/gfx.c](kernel/gfx.c). Switching between them is instant, software-only,
and can't corrupt display state the way mode-switching hardware registers
can — there's no separate "text mode" to fall out of sync with.

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
make run         # boot auroraos.iso through GRUB in QEMU
```

This has to boot through GRUB rather than QEMU's built-in `-kernel`
multiboot loader: the kernel's multiboot header requests a VBE graphics
mode, and it's GRUB, not QEMU directly, that performs the actual BIOS video
mode call on the kernel's behalf. `make run` passes `-vga std` (QEMU's
standard/non-Cirrus VGA+VBE emulation), which is what this was developed
and tested against — Cirrus's emulation is more forgiving of imprecise VGA
register programming in ways that can mask bugs real hardware won't.

Once booted, you'll land in the `aurora:~$` shell prompt. Type `help` to see
available commands, or `gui` to launch the desktop.

## Project layout

- [kernel/boot.s](kernel/boot.s) — Multiboot header (incl. the VBE video
  mode request), entry point, GDT/IDT flush, ISR/IRQ assembly stubs
- [kernel/kernel.c](kernel/kernel.c) — kernel entry point, boot sequence
- [kernel/gdt.c](kernel/gdt.c), [kernel/idt.c](kernel/idt.c),
  [kernel/pic.c](kernel/pic.c) — CPU/interrupt setup
- [kernel/timer.c](kernel/timer.c), [kernel/keyboard.c](kernel/keyboard.c),
  [kernel/mouse.c](kernel/mouse.c) — drivers
- [kernel/font8x8.c](kernel/font8x8.c) — 8x8 bitmap font (ASCII 0x20-0x7E)
- [kernel/gfx.c](kernel/gfx.c) — true-color framebuffer graphics primitives
- [kernel/console.c](kernel/console.c) — software text console (the shell's
  display), rendered on the same framebuffer as the GUI
- [kernel/wm.c](kernel/wm.c) — window manager / compositor and demo apps
- [kernel/shell.c](kernel/shell.c) — interactive shell
- [kernel/memory.c](kernel/memory.c) — multiboot memory map + framebuffer info
- [kernel/linker.ld](kernel/linker.ld) — link script (loads at 1 MiB)
- [scripts/make_iso.sh](scripts/make_iso.sh) — bootable ISO builder

## What this is (and isn't)

This is a real, from-scratch kernel: it manages its own interrupts, drivers,
and display — nothing here rides on Linux or any other existing OS. It's a
single-tasking, single-address-space 32-bit kernel meant as a hobby-OS
starting point, not a production or multi-user operating system. The GUI
runs as a blocking loop inside the shell rather than a separate process —
there's no paging/virtual memory, no filesystem, no process scheduler, and
no userspace program loader yet. Multitasking (so the GUI and shell, or
multiple GUI apps, could run concurrently) is a natural next step if you
want to keep extending it.
