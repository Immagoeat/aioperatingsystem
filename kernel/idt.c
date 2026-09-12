#include "kernel.h"

/* Long-mode IDT gates are 16 bytes (vs 8 in protected mode): the target
 * offset is now a full 64 bits, split across base_low/base_mid/base_high,
 * plus a reserved dword. */
struct idt_entry {
	uint16_t base_low;
	uint16_t sel;
	uint8_t ist;      /* interrupt stack table index; 0 = not used */
	uint8_t flags;
	uint16_t base_mid;
	uint32_t base_high;
	uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr ip;

static isr_t interrupt_handlers[256];

extern void idt_flush(struct idt_ptr *);

#define ISR_DECL(n) extern void isr##n(void);
ISR_DECL(0) ISR_DECL(1) ISR_DECL(2) ISR_DECL(3) ISR_DECL(4)
ISR_DECL(5) ISR_DECL(6) ISR_DECL(7) ISR_DECL(8) ISR_DECL(9)
ISR_DECL(10) ISR_DECL(11) ISR_DECL(12) ISR_DECL(13) ISR_DECL(14)
ISR_DECL(15) ISR_DECL(16) ISR_DECL(17) ISR_DECL(18) ISR_DECL(19)

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

extern void isr128(void); /* syscall gate, see syscall.c */

static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
	idt[num].base_low = base & 0xFFFF;
	idt[num].base_mid = (base >> 16) & 0xFFFF;
	idt[num].base_high = (uint32_t)(base >> 32);
	idt[num].sel = sel;
	idt[num].ist = 0;
	idt[num].flags = flags;
	idt[num].reserved = 0;
}

static const char *exception_messages[] = {
	"Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
	"Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
	"Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
	"Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
	"Coprocessor Fault", "Alignment Check", "Machine Check", "Reserved"
};

void isr_handler(struct registers *regs) {
	if (regs->int_no == 128) {
		syscall_dispatch(regs);
		return;
	}

	if (regs->int_no < 20) {
		kprintf("\n[EXCEPTION] %s (int %d, err %d)\n", exception_messages[regs->int_no], (int)regs->int_no, (int)regs->err_code);
		kprintf("System halted.\n");
		__asm__ volatile("cli");
		for (;;) __asm__ volatile("hlt");
	}
}

void irq_handler(struct registers *regs) {
	if (interrupt_handlers[regs->int_no] != 0) {
		isr_t handler = interrupt_handlers[regs->int_no];
		handler(regs);
	}
	pic_send_eoi(regs->int_no - 32);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
	interrupt_handlers[n] = handler;
}

void idt_install(void) {
	ip.limit = sizeof(struct idt_entry) * 256 - 1;
	ip.base = (uint64_t)(uintptr_t)&idt;

	memset(&idt, 0, sizeof(struct idt_entry) * 256);
	memset(&interrupt_handlers, 0, sizeof(isr_t) * 256);

	pic_remap();

	idt_set_gate(0, (uint64_t)(uintptr_t)isr0, 0x08, 0x8E);
	idt_set_gate(1, (uint64_t)(uintptr_t)isr1, 0x08, 0x8E);
	idt_set_gate(2, (uint64_t)(uintptr_t)isr2, 0x08, 0x8E);
	idt_set_gate(3, (uint64_t)(uintptr_t)isr3, 0x08, 0x8E);
	idt_set_gate(4, (uint64_t)(uintptr_t)isr4, 0x08, 0x8E);
	idt_set_gate(5, (uint64_t)(uintptr_t)isr5, 0x08, 0x8E);
	idt_set_gate(6, (uint64_t)(uintptr_t)isr6, 0x08, 0x8E);
	idt_set_gate(7, (uint64_t)(uintptr_t)isr7, 0x08, 0x8E);
	idt_set_gate(8, (uint64_t)(uintptr_t)isr8, 0x08, 0x8E);
	idt_set_gate(9, (uint64_t)(uintptr_t)isr9, 0x08, 0x8E);
	idt_set_gate(10, (uint64_t)(uintptr_t)isr10, 0x08, 0x8E);
	idt_set_gate(11, (uint64_t)(uintptr_t)isr11, 0x08, 0x8E);
	idt_set_gate(12, (uint64_t)(uintptr_t)isr12, 0x08, 0x8E);
	idt_set_gate(13, (uint64_t)(uintptr_t)isr13, 0x08, 0x8E);
	idt_set_gate(14, (uint64_t)(uintptr_t)isr14, 0x08, 0x8E);
	idt_set_gate(15, (uint64_t)(uintptr_t)isr15, 0x08, 0x8E);
	idt_set_gate(16, (uint64_t)(uintptr_t)isr16, 0x08, 0x8E);
	idt_set_gate(17, (uint64_t)(uintptr_t)isr17, 0x08, 0x8E);
	idt_set_gate(18, (uint64_t)(uintptr_t)isr18, 0x08, 0x8E);
	idt_set_gate(19, (uint64_t)(uintptr_t)isr19, 0x08, 0x8E);

	idt_set_gate(32, (uint64_t)(uintptr_t)irq0, 0x08, 0x8E);
	idt_set_gate(33, (uint64_t)(uintptr_t)irq1, 0x08, 0x8E);
	idt_set_gate(34, (uint64_t)(uintptr_t)irq2, 0x08, 0x8E);
	idt_set_gate(35, (uint64_t)(uintptr_t)irq3, 0x08, 0x8E);
	idt_set_gate(36, (uint64_t)(uintptr_t)irq4, 0x08, 0x8E);
	idt_set_gate(37, (uint64_t)(uintptr_t)irq5, 0x08, 0x8E);
	idt_set_gate(38, (uint64_t)(uintptr_t)irq6, 0x08, 0x8E);
	idt_set_gate(39, (uint64_t)(uintptr_t)irq7, 0x08, 0x8E);
	idt_set_gate(40, (uint64_t)(uintptr_t)irq8, 0x08, 0x8E);
	idt_set_gate(41, (uint64_t)(uintptr_t)irq9, 0x08, 0x8E);
	idt_set_gate(42, (uint64_t)(uintptr_t)irq10, 0x08, 0x8E);
	idt_set_gate(43, (uint64_t)(uintptr_t)irq11, 0x08, 0x8E);
	idt_set_gate(44, (uint64_t)(uintptr_t)irq12, 0x08, 0x8E);
	idt_set_gate(45, (uint64_t)(uintptr_t)irq13, 0x08, 0x8E);
	idt_set_gate(46, (uint64_t)(uintptr_t)irq14, 0x08, 0x8E);
	idt_set_gate(47, (uint64_t)(uintptr_t)irq15, 0x08, 0x8E);

	/* Syscall gate: DPL=3 (flags 0xEE, vs 0x8E's DPL=0 for everything
	 * else) so it's callable via `int 0x80` from a lower privilege
	 * level, matching how a real syscall gate works, even though this
	 * kernel doesn't yet run anything in ring 3 - custom-compiled
	 * programs execute in ring 0 today (see asm.c), so this makes no
	 * practical difference right now but is the architecturally correct
	 * setting to already have in place. */
	idt_set_gate(128, (uint64_t)(uintptr_t)isr128, 0x08, 0xEE);

	idt_flush(&ip);
}
