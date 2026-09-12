# auroraOS assembly language reference

`compile` (see the terminal's `compile SOURCE.asm` command) assembles a
small, deliberately restricted x86-64 assembly language into real, native
machine code — not a bytecode or an interpreted language. `run
PROGRAM.bin` then executes that machine code directly, with **no
isolation from the kernel** (see [SYSCALLS.md](SYSCALLS.md)'s "Program
lifecycle" section for exactly what that means).

This is implemented in [`kernel/asm.c`](../kernel/asm.c). Every
instruction form it supports was individually verified against real GNU
binutils (`as` + `objdump -d`) output before being hand-transcribed into
the assembler — worth knowing if you're extending it, since x86-64
encoding (REX prefixes, ModRM byte field order, immediate
sign-extension) is easy to get subtly and silently wrong.

## Why not just support real x86-64 assembly syntax fully?

It does use standard x86-64 mnemonics and Intel-style operand order
(`op dst, src`), so anything valid in this language is also valid input to
a real assembler like `as`. The restriction is the other direction: this
assembler only understands a small, fixed set of instruction *forms* (see
below) — no general addressing-mode combinatorics, no macros, no
sections, no relocations beyond simple same-file label jumps. That's what
keeps an assembler written from scratch, with no test suite beyond
hand-verification against reference output, tractable to get right.

## Registers

All 16 general-purpose 64-bit registers: `rax rbx rcx rdx rsi rdi rbp rsp
r8 r9 r10 r11 r12 r13 r14 r15`. Every instruction operates on the full
64-bit register — there's no way to address a 32/16/8-bit sub-register
(`eax`, `ax`, `al`, etc.) in this language.

## Instructions

### `mov`

```asm
mov rax, 5          ; register <- immediate (decimal or 0x-prefixed hex)
mov rax, rbx         ; register <- register
mov rax, [rbx]        ; register <- memory at [rbx]
mov rax, [rbx+8]       ; register <- memory at [rbx+8]
mov [rbx], rax        ; memory at [rbx] <- register
mov [rbx+8], rax       ; memory at [rbx+8] <- register
```
Immediates are always encoded as a full 64-bit move (`REX.W B8+reg
imm64`), even for small values — a few bytes larger than what a
size-optimizing assembler like `as` would choose, but a single, simple,
always-correct encoding rule. Memory displacements are always encoded as
a full 32-bit signed displacement, same reasoning.

### Arithmetic & comparison

```asm
add rax, rbx     ; rax += rbx
add rax, 10      ; rax += 10 (immediate, sign-extended from 32 bits)
sub rax, rbx
sub rax, 10
cmp rax, rbx     ; sets flags for a following conditional jump; doesn't modify rax
cmp rax, 10
```

### Stack

```asm
push rax
pop rbx
```

### Control flow

```asm
label_name:
    ...
    jmp label_name    ; unconditional
    je label_name      ; jump if equal (after cmp)
    jne label_name      ; jump if not equal
    jl label_name         ; jump if less (signed)
    jge label_name         ; jump if greater-or-equal (signed)
    jg label_name           ; jump if greater (signed)
    jle label_name            ; jump if less-or-equal (signed)
```
**All jumps are short (8-bit displacement) jumps**, meaning a jump target
must be within about 127 bytes of the instruction after the jump.
`compile` reports "jump target too far" at assembly time if a jump
doesn't fit rather than silently producing a wrong offset or a longer
encoding — keep loops and branches compact, or restructure with a chain
of nearby labels if you hit this.

Labels are just a name followed by a colon, on their own line. Forward
references (jumping to a label defined later in the file) work fine — the
assembler does a full pass to record every label's position before
patching any jump displacements.

### Syscalls

```asm
int80
```
or the alias `syscall` (both assemble to the exact same `int 0x80`
instruction — there is no separate `syscall`-instruction encoding in this
ISA). See [SYSCALLS.md](SYSCALLS.md) for the full syscall ABI and
reference.

### Returning

```asm
ret
```
Also how a program can end without calling `SYS_EXIT` — see
[SYSCALLS.md](SYSCALLS.md#program-lifecycle-how-run-actually-works).

## A complete example

Prints "Hi" and exits with code 0:

```asm
mov rdi, 72     ; 'H'
mov rax, 1      ; SYS_WRITE_CHAR
int80
mov rdi, 105    ; 'i'
mov rax, 1
int80
mov rdi, 0      ; exit code
mov rax, 0      ; SYS_EXIT
int80
```

A countdown loop, printing 5 4 3 2 1:

```asm
mov rax, 5          ; counter
loop_top:
cmp rax, 0
je loop_end
mov rdi, rax
mov rax, 3          ; SYS_WRITE_INT (clobbers rax - see the mov below)
int80
mov rax, rdi        ; restore the counter from rdi into rax
sub rax, 1
jmp loop_top
loop_end:
mov rdi, 0
mov rax, 0
int80
```
(Note the `mov rax, rdi` after the `SYS_WRITE_INT` call: syscalls can
clobber `rax` — the return value always lands there — so a loop counter
kept in `rax` across a syscall needs to be saved somewhere else first,
here round-tripped through `rdi`.)

## Comments

`;` or `#` start a comment that runs to the end of the line. Blank lines
are ignored.

```asm
mov rax, 5   ; this is a comment
# so is this, on its own line
```

## Compiling and running

```
aurora:~$ nano myprogram.asm      ; write your program (Ctrl+S save, Ctrl+X exit)
aurora:~$ compile myprogram.asm   ; -> myprogram.bin
aurora:~$ run myprogram.bin
```
`compile` reports an error with the line number and a short message if
assembly fails (unknown instruction, unknown register, undefined label,
jump too far, duplicate label, or a name too long for FAT16's 8.3 file
naming). `run` reports the program's exit code once it finishes.

## Known limitations

Worth stating plainly rather than glossing over:

- **No string literals or data section.** Every string a program wants to
  print has to be built character-by-character via `SYS_WRITE_CHAR` (or
  `SYS_WRITE_INT` for numbers) — there's no `.ascii "text"`-style
  directive and no way to embed a byte blob in the program to get a
  pointer to for `SYS_WRITE_STR`/`SYS_GFX_TEXT`.
- **Short jumps only** (see above) — no long/near jump encoding.
- **No `call`/function-local labels/scoping** — every label lives in one
  flat namespace for the whole file, and there's no ISA-level notion of
  calling a subroutine (though `push`/`pop`/`jmp` are enough to build one
  by hand if needed).
- **No multiplication, division, bitwise ops, or shifts** — only
  `add`/`sub`/`cmp`.
- **No floating point** at all (the kernel itself never enables
  SSE/x87 — see the note in the main [README](../README.md) about
  `-mgeneral-regs-only`).
- **8.3 filenames only** for both the source and compiled output (a FAT16
  constraint, not an assembler one) — `compile foo.asm` produces
  `foo.bin`, and both names must fit 8 characters + a 3-character
  extension.
