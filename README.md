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
- PS/2 keyboard driver (scancode set 1, shift/caps lock/Ctrl handling)
- PS/2 mouse driver (IRQ12, standard 3-byte packet protocol)
- CMOS Real-Time Clock (RTC) driver for the real wall-clock time — see
  [kernel/rtc.c](kernel/rtc.c)
- A true-color linear-framebuffer graphics library: pixels, lines, rects,
  rounded rects, alpha blending, soft drop shadows, an 8x8 bitmap font
  (scalable), and a back buffer — see [kernel/gfx.c](kernel/gfx.c)
- A software text console rendered on the graphics framebuffer (there is no
  legacy VGA text mode in this design — see [kernel/console.c](kernel/console.c))
- A modern-flat-design windowing GUI: soft shadows, rounded corners,
  macOS-style traffic-light window controls, swappable desktop wallpapers,
  a taskbar with a live wall-clock, an app-launcher search bar, and a few
  demo apps (About, an animated uptime counter, a color palette viewer)
  — see [kernel/wm.c](kernel/wm.c)
- Swappable wallpapers baked in at build time from
  [assets/wallpapers/](assets/wallpapers/) — see [kernel/wallpaper.c](kernel/wallpaper.c)
- A bigger, high-contrast mouse cursor (2x scale, full black outline, drop
  shadow) that stays readable over both light and dark wallpapers
- Minimal freestanding libc (`string.c`, a tiny `printf`)
- Multiboot memory map + framebuffer info parsing
- An interactive shell with builtin commands:
  `help`, `about`, `clear`, `echo`, `mem`, `uptime`, `gui`, `reboot`, `halt`

## The GUI

The desktop launches automatically 3 seconds after the shell starts —
press any key during the countdown to cancel it and stay at the text
prompt instead, or type `gui` yourself at any time. Move the mouse to
control the cursor, drag windows by their title bars, click a window (or
its taskbar button) to bring it to the front, and click the red button in
a title bar to close it. Press `Ctrl+Q` on the keyboard to return to the
text shell at any time.

Unlike the classic "hobby OS" approach of banging VGA CRTC/Sequencer/GC
registers directly to switch video modes, auroraOS never touches legacy VGA
mode registers at all: GRUB sets up a single VBE linear-framebuffer mode
once at boot (requested via the Multiboot header in
[kernel/boot.s](kernel/boot.s)), and both the text console and the windowed
GUI are just two different things drawn onto that same framebuffer with
[kernel/gfx.c](kernel/gfx.c). Switching between them is instant, software-only,
and can't corrupt display state the way mode-switching hardware registers
can — there's no separate "text mode" to fall out of sync with.

### Wallpapers

Click the wallpaper button in the taskbar (or press `Ctrl+W`) to cycle through
the wallpapers in [assets/wallpapers/](assets/wallpapers/). There's no image
decoder in a freestanding kernel, so wallpapers are converted to raw RGB and
baked into the binary at build time by
[tools/img_to_c.py](tools/img_to_c.py) (see the `WALLPAPER_*` variables and
rules in the [Makefile](Makefile)) — they're stored at a fixed 1280x800 and
scaled to whatever the real screen resolution turns out to be at boot via
`gfx_blit_rgb_scaled()`. To add your own: drop a PNG/JPG into
`assets/wallpapers/`, add a `WALLPAPER_GEN`/`WALLPAPER_SRCS` entry and a
generation rule in the Makefile (following the existing `wallpaper_day` /
`wallpaper_night` ones), then register it in the `wallpapers[]` array and
`wallpaper_draw()` switch in [kernel/wallpaper.c](kernel/wallpaper.c).

### Clock

The taskbar shows the real wall-clock time (HH:MM:SS), read from the PC's
CMOS Real-Time Clock — the same battery-backed chip every x86 PC has kept
time on since the original IBM AT, so this reflects the actual system
clock rather than time since boot. It's polled directly (no IRQ), refreshed
once a second in [kernel/wm.c](kernel/wm.c)'s taskbar drawing code via
[kernel/rtc.c](kernel/rtc.c).

### Search

Press `/` or click the magnifying-glass button in the taskbar to open the
app search bar. Type to filter the demo apps by name (case-insensitive,
matches anywhere in the name), press `Enter` or click a result to jump to
it — raising it if it's already open, or (re)launching it if it was
closed, the same way a real OS's app launcher works. `Escape` closes the
search panel without picking anything. Apps are registered once in
`wm_init()`'s `register_app()` calls in [kernel/wm.c](kernel/wm.c); adding
a new one there automatically makes it searchable too.

## Requirements

- `gcc` with 32-bit support (`-m32`)
- GNU `ld`, `binutils`
- `grub-mkrescue` + `xorriso` (from `grub-pc-bin`/`grub2-common` and
  `xorriso`) — preferred; produces a proper hybrid ISO (see below)
- `qemu-system-i386` (to run it)

If `xorriso` isn't available as a system package, `brew install xorriso`
(Homebrew/Linuxbrew, no root needed) works fine. Without it, the build
falls back to a hand-built El Torito-only ISO via `grub-mkimage` +
`genisoimage`/`mkisofs` — that still boots from a real or virtual CD/DVD,
but may not boot from a plain USB stick write (see "Running on real
hardware" below).

## Building

```sh
make kernel      # build kernel_bin/auroraos.bin only
make iso         # build kernel + auroraos.iso (bootable CD image)
```

The ISO is built by [scripts/make_iso.sh](scripts/make_iso.sh), which
prefers `grub-mkrescue` (needs `xorriso`) since it produces a proper
*hybrid* ISO — bootable both as an optical disc and as a raw USB disk
image. If `xorriso`/`grub-mkrescue` aren't available, it falls back to
building the ISO by hand with `grub-mkimage` (targeting
`i386-pc-eltorito`) plus `genisoimage`, which avoids the `xorriso`
dependency entirely but only reliably boots from a CD/DVD.

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

Once booted, you'll land in the `aurora:~$` shell prompt (or straight into
the GUI after the 3-second autoboot countdown). Type `help` to see
available commands, or `gui` to launch the desktop.

## Running on real hardware

`auroraos.iso` is a hybrid image: the same file boots correctly whether
it's burned to a CD/DVD (via the El Torito boot catalog GRUB writes into
it) or written raw to a USB flash drive (via an MBR partition table
`grub-mkrescue` embeds pointing at the ISO's own El Torito data, using
GRUB's `boot_hybrid.img`) — the same mechanism Ubuntu/Debian's own
installer ISOs use. Concretely:

- **USB stick** (the common case on modern PCs, which mostly have no
  optical drive): write the ISO to the whole disk device, not a
  partition, and not "extract the files" — e.g. `sudo dd if=auroraos.iso
  of=/dev/sdX bs=4M status=progress && sync` on Linux/macOS (get the
  right `/dev/sdX` from `lsblk`/`diskutil list` first — this overwrites
  the whole drive), or [Rufus](https://rufus.ie) in "DD Image" mode on
  Windows, or [balenaEtcher](https://etcher.balena.io)/Ventoy on any OS.
- **CD/DVD**: burn `auroraos.iso` as a disc image (not as data files) with
  any burning tool.
- Boot from it: reboot the target PC, enter its boot menu or firmware
  setup (commonly F12, F10, Esc, or Del at power-on) and pick the USB
  drive or optical drive, or set it first in the boot order.
- This targets legacy BIOS / El Torito booting specifically, not UEFI.
  Most PCs still support this via a "Legacy Boot" / "CSM" (Compatibility
  Support Module) option in firmware setup — enable that if the drive
  doesn't show up in the boot menu on a UEFI-only machine.
- It's a real kernel taking over the whole machine: no filesystem access
  to your existing OS, no way back except a reboot/power cycle. Test in
  QEMU first (`make run`) if you want to see it before trying real
  hardware, and don't point it at a drive you care about.

## Project layout

- [kernel/boot.s](kernel/boot.s) — Multiboot header (incl. the VBE video
  mode request), entry point, GDT/IDT flush, ISR/IRQ assembly stubs
- [kernel/kernel.c](kernel/kernel.c) — kernel entry point, boot sequence
- [kernel/gdt.c](kernel/gdt.c), [kernel/idt.c](kernel/idt.c),
  [kernel/pic.c](kernel/pic.c) — CPU/interrupt setup
- [kernel/timer.c](kernel/timer.c), [kernel/keyboard.c](kernel/keyboard.c),
  [kernel/mouse.c](kernel/mouse.c), [kernel/rtc.c](kernel/rtc.c) — drivers
- [kernel/font8x8.c](kernel/font8x8.c) — 8x8 bitmap font (ASCII 0x20-0x7E)
- [kernel/gfx.c](kernel/gfx.c) — true-color framebuffer graphics primitives
- [kernel/console.c](kernel/console.c) — software text console (the shell's
  display), rendered on the same framebuffer as the GUI
- [kernel/wallpaper.c](kernel/wallpaper.c) — wallpaper registry (backed by
  generated sources in `kernel/generated/`, built from
  [assets/wallpapers/](assets/wallpapers/) by [tools/img_to_c.py](tools/img_to_c.py))
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
