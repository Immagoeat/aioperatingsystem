/* boot.s - Multiboot entry point for auroraOS, x86-64.
 *
 * GRUB still loads us via the classic Multiboot 1 protocol, which only
 * promises 32-bit protected mode - there is no "multiboot but 64-bit"
 * hand-off. So _start (32-bit) does the actual work of getting to long
 * mode itself: build minimal page tables, enable PAE, set the long-mode
 * bit in EFER, turn on paging, then load a 64-bit GDT and far-jump into
 * _start64, which is where the real (64-bit) kernel begins.
 */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set VIDMODE,  1<<2
.set FLAGS,    ALIGN | MEMINFO | VIDMODE
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
/* address fields (unused, header_addr etc.) - zero since we don't set
 * the AOUT_KLUDGE flag */
.long 0, 0, 0, 0, 0
/* video mode request: mode_type=0 (linear graphics), width, height, depth.
 * GRUB performs the VBE BIOS call for us before starting the kernel and
 * hands back the framebuffer address via the multiboot info structure. */
.long 0
.long 1024
.long 768
.long 32

/* --- page tables for the long-mode identity map ---
 * One PML4 entry -> 4 PDPT entries -> 4 PD tables of 512 2MB huge pages
 * each (PS bit), identity-mapping the first 4GB of physical address
 * space. The kernel itself only needs the first ~1GB, but a VBE linear
 * framebuffer on real hardware/QEMU commonly gets mapped by the BIOS/PCI
 * BAR far higher (e.g. ~0xFD000000, near the 4GB boundary) - without
 * mapping that too, gfx.c's first write to the framebuffer takes an
 * unhandled page fault (no page fault handler exists yet either) that
 * cascades into a triple fault. 2MB pages throughout rather than 1GB
 * ones, since not every CPU/hypervisor guarantees 1GB page support
 * (pdpe1gb) the way 2MB pages are universally available in long mode.
 * Must be 4K-aligned; .bss so it's zeroed for us at load time. */
.section .bss
.align 4096
pml4_table:
	.skip 4096
pdpt_table:
	.skip 4096
pd_table0:
	.skip 4096
pd_table1:
	.skip 4096
pd_table2:
	.skip 4096
pd_table3:
	.skip 4096

.align 16
stack_bottom:
.skip 65536 /* 64 KiB stack */
stack_top:

.section .text
.code32
.global _start
.type _start, @function
_start:
	cli
	mov $stack_top, %esp
	mov %ebx, %edi          /* stash multiboot info ptr (edi survives below) */

	/* PML4[0] -> pdpt_table (present, writable) */
	mov $pdpt_table, %eax
	or $0x03, %eax
	mov $pml4_table, %ebx
	mov %eax, (%ebx)

	/* PDPT[0..3] -> pd_table0..3 (present, writable), each covering 1GB */
	mov $pdpt_table, %ebx
	mov $pd_table0, %eax
	or $0x03, %eax
	mov %eax, 0(%ebx)
	mov $pd_table1, %eax
	or $0x03, %eax
	mov %eax, 8(%ebx)
	mov $pd_table2, %eax
	or $0x03, %eax
	mov %eax, 16(%ebx)
	mov $pd_table3, %eax
	or $0x03, %eax
	mov %eax, 24(%ebx)

	/* Fill each of the 4 PD tables with 512 2MB pages (present, writable,
	 * huge). %edx carries the running physical address across all four
	 * tables so together they cover a contiguous 0..4GB identity map;
	 * each table is addressed by name explicitly rather than assumed to
	 * be contiguous in memory with the others. */
	mov $0, %edx              /* running physical address */
	mov $pd_table0, %ebx
	call fill_one_pd
	mov $pd_table1, %ebx
	call fill_one_pd
	mov $pd_table2, %ebx
	call fill_one_pd
	mov $pd_table3, %ebx
	call fill_one_pd
	jmp fill_pd_done

fill_one_pd:
	mov $0, %ecx
fill_one_pd_loop:
	mov %edx, %eax
	or $0x83, %eax            /* present | writable | page-size(2MB) */
	mov %eax, (%ebx, %ecx, 8)
	add $0x200000, %edx
	inc %ecx
	cmp $512, %ecx
	jl fill_one_pd_loop
	ret
fill_pd_done:

	/* load CR3 with the PML4 physical address */
	mov $pml4_table, %eax
	mov %eax, %cr3

	/* enable PAE (CR4 bit 5) */
	mov %cr4, %eax
	or $0x20, %eax
	mov %eax, %cr4

	/* set the Long Mode Enable bit in the EFER MSR (0xC0000080, bit 8) */
	mov $0xC0000080, %ecx
	rdmsr
	or $0x100, %eax
	wrmsr

	/* enable paging (CR0 bit 31) - this actually activates long mode
	 * now that LME and PAE are set, but we're still in a 32-bit code
	 * segment (compatibility submode) until the far jump below */
	mov %cr0, %eax
	or $0x80000000, %eax
	mov %eax, %cr0

	lgdt gdt64_ptr
	ljmp $0x08, $_start64

.size _start, . - _start

.code64
.extern kernel_main
_start64:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss

	mov $stack_top, %rsp
	mov %edi, %edi           /* zero-extends edi -> rdi automatically on x86-64 */
	mov %edi, mb_info_ptr(%rip)

	call kernel_main

	cli
hang:
	hlt
	jmp hang

.section .data
.align 8
.global mb_info_ptr
mb_info_ptr:
	.long 0

.align 16
gdt64:
	.quad 0                                   /* null descriptor */
	.quad 0x00AF9A000000FFFF                  /* 64-bit code segment (L=1, present, executable, ring0) */
	.quad 0x00AF92000000FFFF                  /* 64-bit data segment (present, writable, ring0) */
gdt64_end:

gdt64_ptr:
	.word gdt64_end - gdt64 - 1
	.quad gdt64

/* --- ISR/IRQ common stubs (64-bit) --- */
.section .text
.code64

.macro ISR_NOERR num
.global isr\num
isr\num:
	cli
	pushq $0
	pushq $\num
	jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
	cli
	pushq $\num
	jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19

/* Software interrupt used as the syscall gate (see syscall.c). Not a
 * CPU exception, so it's a separate vector from 0-19, but reuses the
 * same isr_common_stub - the C-level isr_handler() tells vector 0x80
 * apart from a real exception by int_no and routes it to the syscall
 * dispatcher instead of the "fatal exception" path. */
ISR_NOERR 128

.macro IRQ num, remapped
.global irq\remapped
irq\remapped:
	cli
	pushq $0
	pushq $\num
	jmp irq_common_stub
.endm

IRQ 32, 0
IRQ 33, 1
IRQ 34, 2
IRQ 35, 3
IRQ 36, 4
IRQ 37, 5
IRQ 38, 6
IRQ 39, 7
IRQ 40, 8
IRQ 41, 9
IRQ 42, 10
IRQ 43, 11
IRQ 44, 12
IRQ 45, 13
IRQ 46, 14
IRQ 47, 15

/* System V AMD64 calling convention: first integer arg goes in %rdi.
 * We pass a pointer to the saved-registers struct there before calling
 * into C, matching kernel.h's `struct registers *`. */
.extern isr_handler
isr_common_stub:
	push %r15
	push %r14
	push %r13
	push %r12
	push %r11
	push %r10
	push %r9
	push %r8
	push %rbp
	push %rdi
	push %rsi
	push %rdx
	push %rcx
	push %rbx
	push %rax

	mov %rsp, %rdi
	call isr_handler

	pop %rax
	pop %rbx
	pop %rcx
	pop %rdx
	pop %rsi
	pop %rdi
	pop %rbp
	pop %r8
	pop %r9
	pop %r10
	pop %r11
	pop %r12
	pop %r13
	pop %r14
	pop %r15

	add $16, %rsp /* pop int_no + err_code */
	sti
	iretq

.extern irq_handler
irq_common_stub:
	push %r15
	push %r14
	push %r13
	push %r12
	push %r11
	push %r10
	push %r9
	push %r8
	push %rbp
	push %rdi
	push %rsi
	push %rdx
	push %rcx
	push %rbx
	push %rax

	mov %rsp, %rdi
	call irq_handler

	pop %rax
	pop %rbx
	pop %rcx
	pop %rdx
	pop %rsi
	pop %rdi
	pop %rbp
	pop %r8
	pop %r9
	pop %r10
	pop %r11
	pop %r12
	pop %r13
	pop %r14
	pop %r15

	add $16, %rsp
	sti
	iretq

/* --- GDT load (for our real, C-managed 64-bit GDT) --- */
.global gdt_flush
gdt_flush:
	lgdt (%rdi)

	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss

	/* reload CS via a far return, since there's no direct 64-bit "jmp
	 * $seg, $offset" form in most assemblers' AT&T syntax */
	pop %rax          /* return address pushed by `call` */
	push $0x08
	push %rax
	lretq

/* --- IDT load --- */
.global idt_flush
idt_flush:
	lidt (%rdi)
	ret

/* --- run a custom-assembled flat binary (see asm.c) ---
 * Saves every callee-saved register the System V ABI requires a
 * function to preserve, calls into the program buffer (address in
 * %rdi, per the calling convention), then restores them - so a
 * well-behaved program that returns via `ret` comes back to C code
 * with the kernel's own register state intact rather than whatever the
 * program left in rbx/rbp/r12-r15. This does NOT protect against a
 * buggy program corrupting memory, jumping somewhere invalid, or
 * disabling interrupts and never returning - there is no isolation
 * here by design (see asm.c's comment on this). */
.global asm_run_trampoline
asm_run_trampoline:
	push %rbx
	push %rbp
	push %r12
	push %r13
	push %r14
	push %r15

	call *%rdi

	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx
	ret
