# auroraOS

A tiny 64-bit (x86-64) hobby operating system kernel, written from scratch in
C and x86 assembly. It boots via GRUB (Multiboot) into a high-resolution
true-color graphics mode and drops you into a software console, with a
modern windowed desktop GUI a `gui` command away.

## Features

- Multiboot-compliant kernel entry, boots via GRUB, then bootstraps itself
  from 32-bit protected mode into 64-bit long mode: GRUB's Multiboot 1
  hand-off only ever promises 32-bit protected mode (there's no "multiboot
  but 64-bit" convention), so `_start` in [kernel/boot.s](kernel/boot.s)
  builds a minimal identity-mapped page table (covering the first 4GB, via
  2MB pages — needed because a VBE linear framebuffer commonly gets mapped
  near the 4GB boundary), enables PAE, sets the long-mode bit in the EFER
  MSR, turns on paging, and far-jumps into a 64-bit code segment before any
  other kernel code runs
- Auto-detected VBE graphics mode: `gfxmode=auto`/`gfxpayload=keep` in
  [scripts/make_iso.sh](scripts/make_iso.sh)'s grub.cfg have GRUB itself
  enumerate the real VBE modes this display reports (checking EDID where the
  BIOS exposes it) and pick the best one, handing that already-active mode
  straight to the kernel; the Multiboot header's fixed 1024x768 request in
  [kernel/boot.s](kernel/boot.s) is now just a fallback for the rare case
  where that doesn't take. There's no real GPU driver behind any of this
  though (just VBE's linear framebuffer handoff), so independent
  multi-monitor output isn't achievable here — GRUB/VBE hands off exactly
  one framebuffer, whichever display it decided was primary.
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
  a taskbar with a live wall-clock, a keyboard-navigable app-launcher search
  bar, a few demo apps (About, an animated uptime counter, a color palette
  viewer), plus a real Text Editor (nano), Terminal (the same shell as the
  text-mode prompt), and a Settings app (timezone, so far) that all run
  full-screen over the desktop — see [kernel/wm.c](kernel/wm.c)
- Swappable wallpapers baked in at build time from
  [assets/wallpapers/](assets/wallpapers/) — see [kernel/wallpaper.c](kernel/wallpaper.c)
- A detailed, smooth-edged mouse cursor: a tapered arrow silhouette at
  native pixel resolution with anti-aliased edges (not a blocky staircase),
  a crisp outline, a subtle highlight, and a soft drop shadow, staying
  readable over both light and dark wallpapers
- Per-window minimize (to the taskbar) and fullscreen (edge-to-edge, no
  chrome) via the title-bar dots, alongside close
- A polling PIO ATA disk driver ([kernel/ata.c](kernel/ata.c)) and a real
  FAT16 filesystem driver ([kernel/fat16.c](kernel/fat16.c)) — auroraOS
  formats a blank attached disk as FAT16 itself on first boot, and every
  file it writes is readable by any normal FAT16 tool (verified against
  Linux's `mtools`), not just by auroraOS itself
- A filesystem-aware terminal (`ls`, `cd`, `mkdir`, `touch`, `rm`, `cat`,
  `echo` with `>` file redirection, and a small full-screen `nano`-style
  editor) — see [kernel/terminal.c](kernel/terminal.c)
- A syscall ABI (`int 0x80`) that custom-compiled programs use to print
  text, draw to the screen, read the keyboard, and more — see
  [kernel/syscall.c](kernel/syscall.c) and
  [docs/SYSCALLS.md](docs/SYSCALLS.md)
- A small hand-written x86-64 assembler and flat-binary loader/runner
  (`compile`/`run` in the terminal) — write a program in auroraOS's
  assembly language, compile it to real native machine code, and run it,
  all from inside the OS — see [kernel/asm.c](kernel/asm.c) and
  [docs/ASSEMBLY.md](docs/ASSEMBLY.md)
- Minimal freestanding libc (`string.c`, a tiny `printf` with 64-bit
  (`%llu`/`%lld`/`%llx`) format support)
- Multiboot memory map + framebuffer info parsing
- An interactive shell with builtin commands:
  `help`, `about`, `clear`, `mem`, `uptime`, `gui`, `reboot`, `halt`, plus
  every terminal.c command above
- Real PCI configuration-space enumeration ([kernel/pci.c](kernel/pci.c)),
  a real driver for the Intel e1000 Ethernet controller
  ([kernel/e1000.c](kernel/e1000.c): MMIO registers, DMA descriptor rings,
  polling TX/RX), and just enough of a network stack
  ([kernel/net.c](kernel/net.c): Ethernet/ARP/IPv4/UDP/DHCP) to request and
  receive a real DHCP lease — `netconnect` in the shell or "Connect" in the
  taskbar's network panel. No Wi-Fi (needs per-chipset firmware-dependent
  drivers and a WPA supplicant — thousands more lines of work), no fake
  "Connected" state for unsupported hardware — see the Network section below
- A real (not simulated) update mechanism given the constraint that there's
  no network stack to fetch updates over: an `update` command
  ([kernel/updatecmd.c](kernel/updatecmd.c)) validates and installs a staged
  kernel binary onto the writable data disk, and GRUB is configured to boot
  it automatically on the next restart instead of the ISO's built-in kernel
  (see the Updates section below)

## The GUI

The desktop launches automatically 3 seconds after the shell starts —
press any key during the countdown to cancel it and stay at the text
prompt instead, or type `gui` yourself at any time. It boots to an empty
screen — nothing is auto-launched — so open what you want via the search
bar. Move the mouse to control the cursor, drag windows by their title
bars, and click a window (or its taskbar button) to bring it to the
front. Press `Ctrl+Q` on the keyboard to return to the text shell at any
time.

Each window has the familiar three title-bar dots: red closes it, yellow
minimizes it (it drops to a taskbar button — click that button again to
bring it back), and green toggles fullscreen (fills the whole desktop,
edge to edge, no rounded corners or shadow — click it again, or the same
button, to restore the window to its previous size and position).

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
app search bar. Type to filter apps by name (case-insensitive, matches
anywhere in the name), use `Up`/`Down` to move the highlighted result, and
press `Enter` (or click a result) to launch it — raising it if it's
already open, or (re)launching it if it was closed, the same way a real
OS's app launcher works. `Escape` closes the search panel without picking
anything. Apps are registered once in `wm_init()`'s `register_app()` /
`register_console_app()` calls in [kernel/wm.c](kernel/wm.c); adding a new
one there automatically makes it searchable too.

### Text Editor, Terminal, and Settings

Three of the searchable apps aren't drawn as windows: **Text Editor**,
**Terminal**, and **Settings** are "console apps" that take over the whole
screen using the same software console the text-mode shell uses, rather
than being painted into a window on the desktop. Launching one suspends
the desktop (the same switch `Ctrl+Q` does), runs full-screen, and
restores the desktop exactly as it was on exit.

- **Text Editor** prompts for a filename (created if it doesn't exist,
  relative to the filesystem root) and opens it in the same `nano`-style
  editor described below.
- **Terminal** drops into the same shell as the text-mode prompt —
  identical commands, same real FAT16 filesystem — except `exit` takes
  its place of `gui` (you're already inside the desktop; `exit` returns to
  it instead of recursing into another copy of it).
- **Settings** currently holds one setting: timezone (see below).

See `run_console_app()` / `is_console_app` in [kernel/wm.c](kernel/wm.c),
`terminal_launch_nano_from_gui()` / `terminal_app_run()` in
[kernel/terminal.c](kernel/terminal.c) and
[kernel/shell.c](kernel/shell.c), and `settings_app_run()` in
[kernel/settings.c](kernel/settings.c).

### Timezone

There's no way for a freestanding kernel on real x86 hardware to actually
detect the user's geographic region — no network stack, no GPS, and the
CMOS RTC itself has no timezone field, just a wall-clock. So "auto-detect"
here is deliberately honest about what's actually possible: the RTC is
assumed to already show local time (the default, a 0-minute offset — i.e.
trust the RTC as-is), and the **Settings** app lets you pick your real UTC
offset from a list of named zones (arrows move, Enter selects, Escape
cancels) if the clock looks wrong. The offset is applied once, centrally,
inside `rtc_get_time()` in [kernel/rtc.c](kernel/rtc.c), so the taskbar
clock (and anything else that reads the time) reflects it automatically.

### Network

Real Wi-Fi (scanning networks, WPA handshakes, per-chipset firmware-
dependent drivers) is thousands of lines of work and not something
auroraOS fakes. What auroraOS has instead is real wired networking:

- [kernel/pci.c](kernel/pci.c) enumerates PCI configuration space (the
  standard 0xCF8/0xCFC mechanism), reads BARs, and can enable a device's
  bus-mastering (needed for DMA) - looking for any class-0x02 (network)
  controller and cross-referencing a small table of chipsets it can name
  (QEMU's emulated e1000/rtl8139/virtio-net, plus a couple of common real
  wired NICs).
- [kernel/e1000.c](kernel/e1000.c) is a real driver for the Intel 8254x
  family (the NIC QEMU emulates by default) - MMIO register access,
  EEPROM-read MAC address, DMA descriptor rings for TX/RX, polling
  send/receive. Only this one chipset is supported; anything else shows up
  in detection but has no driver behind it.
- [kernel/net.c](kernel/net.c) layers just enough of a stack on top -
  Ethernet framing, ARP, IPv4, UDP, and a DHCP client - to request and
  receive a real IP lease. `netconnect` in the shell (or the "Connect"
  button in the taskbar's network panel, wired through
  [kernel/netinfo.c](kernel/netinfo.c)) runs the actual DHCP exchange and
  reports the real result: a genuine leased IP/subnet/gateway/MAC on
  success (verified end-to-end against QEMU's own SLIRP DHCP server - a
  real `10.0.2.x` lease, not a fabricated one), or the real reason it
  failed (no supported hardware, send failure, timeout) - never a fake
  "Connected" state.

One real bug worth noting since it's the kind of thing this stack is easy
to get wrong silently: the TX/RX descriptor rings must be declared
`volatile`, since hardware writes their status fields via DMA outside the
compiler's view of program order. Without it, GCC at `-O2` legally hoists
the status read in `e1000_send()`'s completion-polling loop out of the
loop entirely, spinning on a stale cached value forever - every send
appeared to hang/fail until this was fixed, confirmed by reproducing the
failure with `volatile` removed and fixing it by restoring it.

### Updates

auroraOS boots from a read-only CD-ROM image (`auroraos.iso`) - nothing
running inside it can rewrite that. What it CAN do is write to its own
writable FAT16 data disk, and GRUB (see the `grub.cfg` generated in
[scripts/make_iso.sh](scripts/make_iso.sh)) is configured to look there
first: `search --no-floppy --set=root --file /AURORAOS.UPD` on any attached
disk, booting that if found, falling back to the ISO's built-in kernel
otherwise.

The `update` command ([kernel/updatecmd.c](kernel/updatecmd.c)) is what
gets a new kernel there: it looks for a staged file named `NEWKRNL.BIN` at
the root of the data disk, checks it actually starts with a real Multiboot
header (so it won't install garbage), and if valid, writes it as
`/AURORAOS.UPD` - the exact file GRUB checks for. `reboot` (or a normal
restart) then boots straight into it.

The one honest limitation: auroraOS has no network or USB mass-storage
driver yet, so there's no way for it to fetch `NEWKRNL.BIN` from anywhere
on its own. Getting a new kernel build onto the data disk today means
copying it there from the host machine - the same way the disk image
itself gets created (e.g. with mtools' `mcopy -i auroraos_disk.img
kernel_bin/auroraos.bin ::NEWKRNL.BIN`, or by mounting the image directly).
Once it's there, `update` and the reboot are real, not simulated - this was
verified end-to-end (install, reboot, GRUB finding and booting the new
kernel from the data disk, filesystem left intact).

## Filesystem, terminal, and custom programs

auroraOS's persistent storage is a second, separate disk from the ISO you
boot from — the ISO is a read-only CD-ROM image; files need a real
writable disk. `make disk` creates a blank 64MB raw disk image
(`auroraos_disk.img`, via [scripts/make_disk.sh](scripts/make_disk.sh));
`make run` attaches it automatically. The very first boot with a blank
disk attached formats it as FAT16 (`[boot] No filesystem found;
formatting disk as FAT16...` in the boot log); every boot after that just
mounts the existing filesystem, so your files persist across reboots —
verified by writing a file, rebooting, and reading it back. The disk
survives `make clean`; run `make clean-disk` if you actually want to wipe
it back to blank.

The driver looks for the disk on the primary ATA bus's **slave** position
(port 0x1F0, drive-select bit set) — deliberately not the master, which is
commonly where a boot CD-ROM's ATAPI drive sits. `make run` attaches it as
QEMU drive `index=1` with `-boot d` (boot from the CD-ROM first,
regardless of what a hard disk being present might otherwise default to).

Once at the `aurora:~$` prompt:

```
aurora:~$ touch hello.txt
aurora:~$ echo Hello, auroraOS! > hello.txt
aurora:~$ cat hello.txt
Hello, auroraOS!
aurora:~$ ls
HELLO.TXT  (17 bytes)
```

`cd` supports exactly one level of subdirectories (`mkdir foo`, `cd foo`,
`cd ..`) — FAT16 itself supports arbitrary nesting, but only this shallower
shape has actually been built and tested here, and claiming more would
be exactly the kind of untested corner this project tries hard to avoid.
Filenames follow FAT16's classic 8.3 rule (up to 8 characters, an optional
3-character extension, automatically uppercased) — `mkdir`/`touch`/`echo
>`/`compile` all report an error rather than silently truncating or
corrupting a name that doesn't fit.

`nano FILENAME` opens a small full-screen text editor — enough to write or
revise a short program, deliberately not a full nano clone (there's no
cursor movement back into earlier text; see the "editing model" note in
its help line). `Ctrl+S` saves, `Ctrl+X` exits (prompting to save first if
there are unsaved changes).

`compile SOURCE.asm` assembles auroraOS's own small assembly language
(documented in full in [docs/ASSEMBLY.md](docs/ASSEMBLY.md)) into a real
native x86-64 flat binary; `run PROGRAM.bin` executes it directly. There's
**no process isolation** — a compiled program runs with the same
privileges as the kernel itself, so a bug in one can genuinely crash or
corrupt the running OS, the same way a bug in kernel code could. Programs
talk to the kernel (printing, drawing, reading the keyboard, timing) via a
real `int 0x80` syscall gate — see [docs/SYSCALLS.md](docs/SYSCALLS.md)
for the full ABI reference and [docs/RENDERING.md](docs/RENDERING.md) for
the graphics API those syscalls expose (and the fuller C API underneath,
for kernel code itself).

## Requirements

- `gcc` with x86-64 support (`-m64` — the default on essentially any Linux
  gcc build)
- GNU `ld`, `binutils`
- `grub-mkrescue` + `xorriso` (from `grub-pc-bin`/`grub2-common` and
  `xorriso`) — preferred; produces a proper hybrid ISO (see below)
- `qemu-system-x86_64` (to run it — `qemu-system-i386` can't execute the
  64-bit long-mode code this kernel switches itself into during boot)

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
make run         # build (if needed) + boot auroraos.iso and auroraos_disk.img in QEMU
```

`make run` builds and boots the ISO *and* the persistent data disk
together (creating `auroraos_disk.img` via `make disk` if it doesn't
already exist) — see "Filesystem, terminal, and custom programs" above.

This has to boot through GRUB rather than QEMU's built-in `-kernel`
multiboot loader: the kernel's multiboot header requests a VBE graphics
mode, and it's GRUB, not QEMU directly, that performs the actual BIOS video
mode call on the kernel's behalf. `make run` passes `-vga std` (QEMU's
standard/non-Cirrus VGA+VBE emulation), which is what this was developed
and tested against — Cirrus's emulation is more forgiving of imprecise VGA
register programming in ways that can mask bugs real hardware won't.
It also explicitly attaches an emulated Intel e1000 NIC on QEMU's
user-mode (SLIRP) network backend, so `netconnect`/the network panel have
real hardware to find and a real DHCP server to lease from.

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
- It's a real kernel taking over the whole machine: no access to your
  existing OS's files (auroraOS only reads/writes its own separate FAT16
  disk — see "Filesystem, terminal, and custom programs" above), no way
  back except a reboot/power cycle. Test in QEMU first (`make run`) if you
  want to see it before trying real hardware, and don't point auroraOS's
  data-disk driver at a real hard drive you care about — it will format
  whatever's attached at the primary-slave ATA position if that disk
  doesn't already look like a valid FAT16 volume.

## Project layout

- [kernel/boot.s](kernel/boot.s) — Multiboot header (incl. the VBE video
  mode request), entry point, GDT/IDT flush, ISR/IRQ assembly stubs
- [kernel/kernel.c](kernel/kernel.c) — kernel entry point, boot sequence
- [kernel/gdt.c](kernel/gdt.c), [kernel/idt.c](kernel/idt.c),
  [kernel/pic.c](kernel/pic.c) — CPU/interrupt setup
- [kernel/timer.c](kernel/timer.c), [kernel/keyboard.c](kernel/keyboard.c),
  [kernel/mouse.c](kernel/mouse.c), [kernel/rtc.c](kernel/rtc.c),
  [kernel/ata.c](kernel/ata.c) — drivers
- [kernel/fat16.c](kernel/fat16.c) — FAT16 filesystem driver
- [kernel/terminal.c](kernel/terminal.c) — filesystem-aware shell commands
  (ls/cd/mkdir/touch/rm/cat/echo/nano/compile/run)
- [kernel/syscall.c](kernel/syscall.c) — the `int 0x80` syscall ABI (see
  [docs/SYSCALLS.md](docs/SYSCALLS.md))
- [kernel/asm.c](kernel/asm.c) — the custom x86-64 assembler + flat-binary
  runner (see [docs/ASSEMBLY.md](docs/ASSEMBLY.md))
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
- [scripts/make_disk.sh](scripts/make_disk.sh) — blank data disk image creator
- [docs/SYSCALLS.md](docs/SYSCALLS.md), [docs/ASSEMBLY.md](docs/ASSEMBLY.md),
  [docs/RENDERING.md](docs/RENDERING.md) — reference docs for the syscall
  ABI, the custom assembly language, and the graphics API

## What this is (and isn't)

This is a real, from-scratch kernel: it manages its own interrupts,
drivers, filesystem, and display — nothing here rides on Linux or any
other existing OS, and files it writes are real, verified-correct FAT16
data (readable by any normal FAT16 tool, not just by auroraOS). It's a
single-tasking, 64-bit kernel meant as a hobby-OS starting point, not a
production or multi-user operating system. Paging exists (long mode
requires it) but only as a flat identity map set up once at boot — there's
no per-process address space, no memory protection, and no process
scheduler. Custom-compiled programs run with the same privileges as the
kernel itself (see [docs/SYSCALLS.md](docs/SYSCALLS.md)'s "Program
lifecycle" section) rather than in an isolated process — there's no
concept of a "process" here at all, just the kernel calling directly into
a program's code and back. The GUI similarly runs as a blocking loop
inside the shell rather than a separate process. Multitasking (so the GUI
and shell, or multiple GUI apps or compiled programs, could run
concurrently) and real memory protection (per-process page tables,
ring 3 execution) are natural next steps if you want to keep extending it.
