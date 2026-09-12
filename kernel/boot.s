/* boot.s - Multiboot entry point for auroraOS */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KiB stack */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
	mov $stack_top, %esp
	movl %ebx, mb_info_ptr   /* save multiboot info pointer */

	call kernel_main

	cli
hang:
	hlt
	jmp hang
.size _start, . - _start

.section .data
.global mb_info_ptr
mb_info_ptr:
	.long 0

/* --- ISR/IRQ common stubs --- */
.section .text
.macro ISR_NOERR num
.global isr\num
isr\num:
	cli
	push $0
	push $\num
	jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
	cli
	push $\num
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

.macro IRQ num, remapped
.global irq\remapped
irq\remapped:
	cli
	push $0
	push $\num
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

.extern isr_handler
isr_common_stub:
	pusha
	mov %ds, %ax
	push %eax

	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	push %esp
	call isr_handler
	add $4, %esp

	pop %eax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	popa
	add $8, %esp
	sti
	iret

.extern irq_handler
irq_common_stub:
	pusha
	mov %ds, %ax
	push %eax

	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	push %esp
	call irq_handler
	add $4, %esp

	pop %eax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	popa
	add $8, %esp
	sti
	iret

/* --- GDT flush --- */
.global gdt_flush
gdt_flush:
	mov 4(%esp), %eax
	lgdt (%eax)

	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss

	jmp $0x08, $.flush
.flush:
	ret

/* --- IDT load --- */
.global idt_flush
idt_flush:
	mov 4(%esp), %eax
	lidt (%eax)
	ret
