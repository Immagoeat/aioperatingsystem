#include "kernel.h"

extern uint32_t mb_info_ptr;

static void print_banner(void) {
	gfx_color_t accent = console_color_accent();
	console_set_color(accent);
	console_writestring("    _                                 ____   _____\n");
	console_writestring("   / \\  _   _ _ __ ___  _ __ __ _     / __ \\ / ____|\n");
	console_writestring("  / _ \\| | | | '__/ _ \\| '__/ _` |   | |  | | (___  \n");
	console_writestring(" / ___ \\ |_| | | | (_) | | | (_| |   | |  | |\\___ \\ \n");
	console_writestring("/_/   \\_\\__,_|_|  \\___/|_|  \\__,_|    \\____/ ____) |\n");
	console_writestring("                                            |_____/\n");
	console_set_color(console_color_default());
	console_writestring("\n");
}

static void halt_with_message(const char *msg) {
	/* Framebuffer init failed - we have no console to draw to, so this
	 * is one of the few spots left that has nothing to say anything
	 * with. Just halt; a future revision could fall back to legacy VGA
	 * text mode here if it becomes worth the complexity. */
	(void)msg;
	__asm__ volatile("cli");
	for (;;) __asm__ volatile("hlt");
}

void kernel_main(void) {
	/* memory_init() must run before gfx_init(): the framebuffer address
	 * GRUB handed us lives in the multiboot info structure. */
	memory_init(mb_info_ptr);

	if (!gfx_init()) {
		halt_with_message("no linear framebuffer available");
	}
	gfx_set_font_scale(1);
	console_init();

	console_set_color(GFX_RGB(0xFF, 0xC1, 0x4E));
	console_writestring("auroraOS booting...\n\n");
	console_set_color(console_color_default());

	print_banner();

	kprintf("[boot] Framebuffer: %dx%d\n", gfx_width(), gfx_height());

	kprintf("[boot] Installing GDT...\n");
	gdt_install();

	kprintf("[boot] Installing IDT...\n");
	idt_install();

	kprintf("[boot] Installing PIT timer (100Hz)...\n");
	timer_install();

	kprintf("[boot] Installing PS/2 keyboard driver...\n");
	keyboard_install();

	kprintf("[boot] Installing PS/2 mouse driver...\n");
	mouse_install();

	kprintf("[boot] Enabling interrupts...\n");
	__asm__ volatile("sti");

	kprintf("[boot] Detecting data disk...\n");
	if (ata_init()) {
		if (fat16_mount()) {
			kprintf("[boot] FAT16 filesystem mounted.\n");
		} else {
			kprintf("[boot] No filesystem found; formatting disk as FAT16...\n");
			uint32_t sectors = ata_get_total_sectors();
			if (fat16_format(sectors) && fat16_mount()) {
				kprintf("[boot] Disk formatted and mounted.\n");
			} else {
				kprintf("[boot] Disk format failed; file commands will be unavailable.\n");
			}
		}
	} else {
		kprintf("[boot] No data disk found; file commands will be unavailable.\n");
	}

	kprintf("[boot] Total memory: %u KB\n", memory_total_kb());
	kprintf("\nauroraOS is ready.\n");
	console_set_color(console_color_dim());
	console_writestring("Type 'help' to see available commands. Type 'gui' to start the desktop.\n\n");
	console_set_color(console_color_default());

	shell_run();
}
