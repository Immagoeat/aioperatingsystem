# auroraOS syscall reference

Custom-compiled programs (see [ASSEMBLY.md](ASSEMBLY.md)) run as plain
native x86-64 machine code with **no isolation from the kernel** — there is
no user/kernel privilege separation, no process model, and no memory
protection in auroraOS today. The syscall mechanism exists anyway, in the
same shape a real OS's would, because it's the correct place to draw the
line between "things a program does itself" (arithmetic, control flow) and
"things only the kernel can do" (touch the screen, read the keyboard, wait
on time) — and because it's what `int 0x80` and this exact calling
convention are for on any real x86 Unix-like system.

## Calling convention

```
rax = syscall number
rdi = argument 1 (if any)
rsi = argument 2 (if any)
rdx = argument 3 (if any)
r10 = argument 4 (if any)   <- NOT rcx; see note below
--- int 0x80 ---
rax = return value
```

Every syscall takes **at most 4 arguments**, all plain 64-bit
integers/pointers. None of them pack multiple values into one register —
see [`SYS_GFX_RECT`](#sys_gfx_rect-8) and [`SYS_GFX_LINE`](#sys_gfx_line-9)
below for how operations that conceptually need more than 4 values are
split instead. This is deliberate: it keeps the ABI simple enough to use by
hand in assembly and to document exhaustively, rather than growing
register-packing conventions per-syscall.

`r10` rather than `rcx` for the 4th argument matches the real Linux x86-64
`syscall` convention, where `rcx` gets clobbered by the `syscall`
instruction itself; that isn't actually true of `int 0x80` (nothing here
clobbers `rcx`), but `r10` was kept anyway for familiarity.

In auroraOS's assembly language, invoke a syscall with the `int80`
mnemonic (or the alias `syscall`, which does the exact same thing — there
is no dedicated `syscall` *instruction* form of this ABI, only the `int
0x80` gate):

```asm
mov rax, 1        ; SYS_WRITE_CHAR
mov rdi, 72       ; 'H'
int80
```

## Where this lives in the kernel

- The gate itself: `isr128` in [`kernel/boot.s`](../kernel/boot.s), which
  reuses the same register-saving stub as every CPU exception/IRQ handler.
- Routing `int_no == 128` to the syscall path instead of the fatal
  exception path: `isr_handler()` in [`kernel/idt.c`](../kernel/idt.c).
- The actual dispatch table: [`kernel/syscall.c`](../kernel/syscall.c).
- The `SYS_*` numeric constants: [`kernel/kernel.h`](../kernel/kernel.h).

## Syscall reference

### `SYS_EXIT` (0)

Ends the running program immediately.

| Register | Meaning |
|---|---|
| `rdi` | exit code |

Does not return to the instruction after `int80` the way every other
syscall does — control returns instead to whatever called
`asm_run()` (the terminal's `run` command), with the exit code available
there. See the "Program lifecycle" note below for how this actually works
under the hood.

```asm
mov rdi, 0     ; exit code 0
mov rax, 0     ; SYS_EXIT
int80
```

### `SYS_WRITE_CHAR` (1)

Prints one character to the console.

| Register | Meaning |
|---|---|
| `rdi` | character code (0-255) |

```asm
mov rdi, 72    ; 'H'
mov rax, 1
int80
```

### `SYS_WRITE_STR` (2)

Prints a null-terminated string to the console.

| Register | Meaning |
|---|---|
| `rdi` | pointer to a null-terminated string |

There is no way to embed a string literal directly in auroraOS's assembly
language today (see [ASSEMBLY.md](ASSEMBLY.md)'s "Known limitations")
— this syscall exists and is documented for completeness and for future
use once string literals/data sections are added, but `SYS_WRITE_CHAR` in
a loop is the only way to print text from hand-written assembly right now.

### `SYS_WRITE_INT` (3)

Prints a signed integer in decimal.

| Register | Meaning |
|---|---|
| `rdi` | the integer to print (sign-extended from the low 32 bits) |

```asm
mov rdi, 42
mov rax, 3
int80          ; prints "42"
```

### `SYS_READ_CHAR` (4)

Blocks until a key is pressed, then returns its character code.

| Return (`rax`) | Meaning |
|---|---|
| low byte | the character read |

```asm
mov rax, 4
int80          ; rax now holds the character typed
```

### `SYS_HAS_KEY` (5)

Checks whether a keypress is waiting, without blocking.

| Return (`rax`) | Meaning |
|---|---|
| `1` | a key is available (safe to call `SYS_READ_CHAR` without blocking) |
| `0` | no key waiting |

### `SYS_GFX_SET_EXTRA` (6)

Stages two extra values for the next `SYS_GFX_RECT` or `SYS_GFX_LINE`
call. See those two syscalls for why this exists.

| Register | Meaning |
|---|---|
| `rdi` | extra value A |
| `rsi` | extra value B |

### `SYS_GFX_RECT` (8)

Fills a rectangle on screen. Needs 5 conceptual values (x, y, width,
height, color) — one more than the 4-argument limit — so width and height
are staged first with `SYS_GFX_SET_EXTRA`.

| Register | Meaning |
|---|---|
| `rdi` | x |
| `rsi` | y |
| `rdx` | color (0x00RRGGBB) |
| *(staged)* | width, height, via `SYS_GFX_SET_EXTRA(width, height)` |

```asm
mov rdi, 100          ; width
mov rsi, 50           ; height
mov rax, 6            ; SYS_GFX_SET_EXTRA
int80

mov rdi, 10           ; x
mov rsi, 10           ; y
mov rdx, 0xFF0000     ; red
mov rax, 8            ; SYS_GFX_RECT
int80
```

### `SYS_GFX_PIXEL` (7)

Sets a single pixel's color.

| Register | Meaning |
|---|---|
| `rdi` | x |
| `rsi` | y |
| `rdx` | color (0x00RRGGBB) |

### `SYS_GFX_LINE` (9)

Draws a line from (x0, y0) to (x1, y1). Like `SYS_GFX_RECT`, the endpoint
is staged first since a line needs 5 values.

| Register | Meaning |
|---|---|
| `rdi` | x0 |
| `rsi` | y0 |
| `rdx` | color (0x00RRGGBB) |
| *(staged)* | x1, y1, via `SYS_GFX_SET_EXTRA(x1, y1)` |

### `SYS_GFX_TEXT` (10)

Draws a null-terminated string as text.

| Register | Meaning |
|---|---|
| `rdi` | x |
| `rsi` | y |
| `rdx` | pointer to a null-terminated string |
| `r10` | color (0x00RRGGBB) |

Subject to the same "no string literals in assembly yet" limitation as
`SYS_WRITE_STR` above.

### `SYS_GFX_FLIP` (11)

Presents everything drawn since the last flip to the actual screen. Every
`SYS_GFX_*` drawing call writes to an off-screen back buffer (see
[RENDERING.md](RENDERING.md)); nothing is visible until this is called.

No arguments.

### `SYS_GFX_WIDTH` (12) / `SYS_GFX_HEIGHT` (13)

Returns the screen's current width or height in pixels, in `rax`. Query
these rather than hardcoding a resolution — the actual screen size depends
on what GRUB negotiated with the BIOS at boot (see the main
[README](../README.md)) and isn't guaranteed to be any particular value.

### `SYS_GET_TICKS` (14)

Returns the number of timer ticks since boot, in `rax`. The timer runs at
100Hz (see [`kernel/timer.c`](../kernel/timer.c)), so ticks / 100 = seconds
since boot.

### `SYS_SLEEP_TICKS` (15)

Blocks the calling program for the given number of timer ticks (1 tick =
10ms at the kernel's 100Hz rate).

| Register | Meaning |
|---|---|
| `rdi` | number of ticks to wait |

## Program lifecycle (how `run` actually works)

A compiled program is loaded into a fixed buffer and entered with a direct
`call` (see `asm_run()` / `asm_run_trampoline` in
[`kernel/asm.c`](../kernel/asm.c) / [`kernel/boot.s`](../kernel/boot.s)).
It's expected to eventually either:

- execute `ret`, which returns control to the terminal normally (the exit
  code in this case is whatever's left in `rax`, which is *not*
  necessarily meaningful unless the program set it deliberately), or
- call `SYS_EXIT`, which is the reliable way to report a specific exit
  code and stop immediately regardless of how deep in the program's own
  control flow it's called from.

Because there's no separate process/stack for the program, `SYS_EXIT`
works by rewriting the interrupt frame's return address to point at a
single `ret` instruction rather than back into the program — so the
`iretq` that ends the syscall handler effectively performs the program's
own "return to caller" for it. This is documented here because it's a
genuinely unusual mechanism worth understanding if you're modifying
`syscall.c` or `asm.c`, not because a program author needs to know it to
use `SYS_EXIT`.

**There is no sandboxing.** A program that writes to an invalid address,
jumps somewhere nonsensical, or executes a privileged/undefined
instruction can crash or corrupt the kernel exactly the same way a bug in
kernel code itself could — there's no separate address space or privilege
ring protecting the kernel from a buggy or malicious compiled program. See
[ASSEMBLY.md](ASSEMBLY.md) for what the instruction set actually lets a
program do.
