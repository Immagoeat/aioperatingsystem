#include "kernel.h"

static volatile uint32_t tick = 0;

static void timer_callback(struct registers *regs) {
	(void)regs;
	tick++;
}

void timer_install(void) {
	register_interrupt_handler(32, timer_callback);

	uint32_t frequency = 100; /* 100 Hz */
	uint32_t divisor = 1193180 / frequency;

	outb(0x43, 0x36);
	outb(0x40, (uint8_t)(divisor & 0xFF));
	outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint32_t timer_get_ticks(void) {
	return tick;
}

void timer_wait(uint32_t ticks) {
	uint32_t end = tick + ticks;
	while (tick < end) {
		__asm__ volatile("hlt");
	}
}
