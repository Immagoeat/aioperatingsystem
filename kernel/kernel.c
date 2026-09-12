#include "kernel.h"

extern uint32_t mb_info_ptr;

static void print_banner(void) {
	terminal_setcolor(0x0B);
	terminal_writestring("    _                                 ____   _____\n");
	terminal_writestring("   / \\  _   _ _ __ ___  _ __ __ _     / __ \\ / ____|\n");
	terminal_writestring("  / _ \\| | | | '__/ _ \\| '__/ _` |   | |  | | (___  \n");
	terminal_writestring(" / ___ \\ |_| | | | (_) | | | (_| |   | |  | |\\___ \\ \n");
	terminal_writestring("/_/   \\_\\__,_|_|  \\___/|_|  \\__,_|    \\____/ ____) |\n");
	terminal_writestring("                                            |_____/\n");
	terminal_setcolor(0x0F);
	terminal_writestring("\n");
}

void kernel_main(void) {
	terminal_initialize();

	terminal_setcolor(0x0E);
	terminal_writestring("auroraOS booting...\n\n");
	terminal_setcolor(0x0F);

	print_banner();

	kprintf("[boot] Installing GDT...\n");
	gdt_install();

	kprintf("[boot] Installing IDT...\n");
	idt_install();

	kprintf("[boot] Initializing memory map...\n");
	memory_init(mb_info_ptr);

	kprintf("[boot] Installing PIT timer (100Hz)...\n");
	timer_install();

	kprintf("[boot] Installing PS/2 keyboard driver...\n");
	keyboard_install();

	kprintf("[boot] Enabling interrupts...\n");
	__asm__ volatile("sti");

	kprintf("[boot] Total memory: %u KB\n", memory_total_kb());
	kprintf("\nauroraOS is ready.\n");
	terminal_setcolor(0x08);
	terminal_writestring("Type 'help' to see available commands.\n\n");
	terminal_setcolor(0x0F);

	shell_run();
}
