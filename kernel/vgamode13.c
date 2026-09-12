/* vgamode13.c - switch VGA into mode 13h (320x200, 256 colors, linear
 * framebuffer at 0xA0000) by programming the CRTC/sequencer/graphics
 * controller registers directly. No BIOS calls are available here since
 * we're already in 32-bit protected mode by the time the kernel runs. */
#include "kernel.h"

#define VGA_AC_INDEX 0x3C0
#define VGA_AC_WRITE 0x3C0
#define VGA_AC_READ  0x3C1
#define VGA_MISC_WRITE 0x3C2
#define VGA_SEQ_INDEX 0x3C4
#define VGA_SEQ_DATA  0x3C5
#define VGA_DAC_READ_INDEX  0x3C7
#define VGA_DAC_WRITE_INDEX 0x3C8
#define VGA_DAC_DATA        0x3C9
#define VGA_MISC_READ 0x3CC
#define VGA_GC_INDEX 0x3CE
#define VGA_GC_DATA  0x3CF
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5
#define VGA_INSTAT_READ 0x3DA

/* Fallback table (approximate standard mode 3 register values), used only
 * if we're asked to restore text mode without ever having snapshotted it. */
static const unsigned char g_80x25_text[] = {
/* MISC */
	0x67,
/* SEQ */
	0x03, 0x00, 0x03, 0x00, 0x02,
/* CRTC */
	0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
	0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
	0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
	0xFF,
/* GC */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00,
	0xFF,
/* AC */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	0x0C, 0x00, 0x0F, 0x08, 0x00
};

/* Snapshot of the real text-mode registers, captured at boot (before we
 * ever touch VGA mode) so vga_set_text_mode() can restore exactly what
 * GRUB/BIOS set up rather than relying on a hand-typed guess. */
#define REGS_SIZE (1 + 5 + 25 + 9 + 21)
static unsigned char saved_text_regs[REGS_SIZE];
static bool have_saved_text_regs = false;

static void read_regs(unsigned char *regs) {
	*regs++ = inb(VGA_MISC_READ);

	for (uint8_t i = 0; i < 5; i++) {
		outb(VGA_SEQ_INDEX, i);
		*regs++ = inb(VGA_SEQ_DATA);
	}

	for (uint8_t i = 0; i < 25; i++) {
		outb(VGA_CRTC_INDEX, i);
		*regs++ = inb(VGA_CRTC_DATA);
	}

	for (uint8_t i = 0; i < 9; i++) {
		outb(VGA_GC_INDEX, i);
		*regs++ = inb(VGA_GC_DATA);
	}

	for (uint8_t i = 0; i < 21; i++) {
		(void)inb(VGA_INSTAT_READ);
		outb(VGA_AC_INDEX, i);
		*regs++ = inb(VGA_AC_READ);
	}
}

void vga_snapshot_current_mode(void) {
	read_regs(saved_text_regs);
	have_saved_text_regs = true;
}

static const unsigned char g_320x200x256[] = {
/* MISC */
	0x63,
/* SEQ */
	0x03, 0x01, 0x0F, 0x00, 0x0E,
/* CRTC */
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
	0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
	0xFF,
/* GC */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
	0xFF,
/* AC */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x41, 0x00, 0x0F, 0x00, 0x00
};

static void write_regs(const unsigned char *regs) {
	/* MISC */
	outb(VGA_MISC_WRITE, *regs++);

	/* SEQ */
	for (uint8_t i = 0; i < 5; i++) {
		outb(VGA_SEQ_INDEX, i);
		outb(VGA_SEQ_DATA, *regs++);
	}

	/* unlock CRTC registers 0-7 (clear the protect bit) */
	outb(VGA_CRTC_INDEX, 0x11);
	outb(VGA_CRTC_DATA, inb(VGA_CRTC_DATA) & ~0x80);

	/* CRTC */
	for (uint8_t i = 0; i < 25; i++) {
		outb(VGA_CRTC_INDEX, i);
		outb(VGA_CRTC_DATA, *regs++);
	}

	/* GC */
	for (uint8_t i = 0; i < 9; i++) {
		outb(VGA_GC_INDEX, i);
		outb(VGA_GC_DATA, *regs++);
	}

	/* AC */
	for (uint8_t i = 0; i < 21; i++) {
		(void)inb(VGA_INSTAT_READ); /* reset flip-flop */
		outb(VGA_AC_INDEX, i);
		outb(VGA_AC_WRITE, *regs++);
	}

	(void)inb(VGA_INSTAT_READ);
	outb(VGA_AC_INDEX, 0x20); /* enable video output */
}

void vga_set_mode13h(void) {
	write_regs(g_320x200x256);
}

/* Switching into mode 13h (chained 256-color mode) and back scrambles the
 * font glyph data that lives in plane 2 of VGA memory, because mode 13h
 * writes touch all four planes through the chain-4 addressing logic. Text
 * mode reads glyph bitmaps back out of plane 2, so without reloading it
 * the screen comes back as garbage instead of readable text. This
 * reloads our own 8x8 font into plane 2 using the standard VGA trick of
 * temporarily pointing the CPU window at plane 2 only. */
static void reload_text_mode_font(void) {
	uint8_t *fb = (uint8_t *)0xA0000;

	/* Sequencer: disable even/odd, enable writes to plane 2 only */
	outb(VGA_SEQ_INDEX, 0x02);
	outb(VGA_SEQ_DATA, 0x04); /* map mask: plane 2 */
	outb(VGA_SEQ_INDEX, 0x04);
	outb(VGA_SEQ_DATA, 0x06); /* sequential addressing, extended memory */

	/* Graphics controller: select plane 2 for reads, disable odd/even,
	 * point the window at 0xA0000-0xAFFFF */
	outb(VGA_GC_INDEX, 0x04);
	outb(VGA_GC_DATA, 0x02);
	outb(VGA_GC_INDEX, 0x05);
	outb(VGA_GC_DATA, 0x00);
	outb(VGA_GC_INDEX, 0x06);
	outb(VGA_GC_DATA, 0x04);

	for (int ch = 0; ch < 256; ch++) {
		const uint8_t *glyph = font8x8_get_glyph((char)ch);
		for (int row = 0; row < 8; row++) {
			fb[ch * 32 + row] = glyph[row];
		}
	}

	/* Restore normal read/write mode (plane 0/1 text + attribute access) */
	outb(VGA_SEQ_INDEX, 0x02);
	outb(VGA_SEQ_DATA, 0x03);
	outb(VGA_SEQ_INDEX, 0x04);
	outb(VGA_SEQ_DATA, 0x02);

	outb(VGA_GC_INDEX, 0x04);
	outb(VGA_GC_DATA, 0x00);
	outb(VGA_GC_INDEX, 0x05);
	outb(VGA_GC_DATA, 0x10);
	outb(VGA_GC_INDEX, 0x06);
	outb(VGA_GC_DATA, 0x0E);
}

/* Mode 13h uses "chain-4" addressing: each consecutive pixel byte the CPU
 * writes actually lands in a different one of the 4 VGA planes, round
 * robin. Whatever we drew is therefore scattered across all 4 planes, not
 * just "plane 0". Text mode reads plane 0 for character codes and plane 1
 * for attributes, so leftover chain-4 pixel garbage shows up as scrambled
 * text unless every plane is explicitly cleared before switching back. */
static void clear_all_planes(void) {
	uint8_t *fb = (uint8_t *)0xA0000;

	outb(VGA_SEQ_INDEX, 0x04);
	outb(VGA_SEQ_DATA, 0x06); /* sequential addressing (disable chain-4) */

	for (int plane = 0; plane < 4; plane++) {
		outb(VGA_SEQ_INDEX, 0x02);
		outb(VGA_SEQ_DATA, (uint8_t)(1 << plane));
		memset(fb, 0x00, 0x10000);
	}

	outb(VGA_SEQ_INDEX, 0x02);
	outb(VGA_SEQ_DATA, 0x03);
	outb(VGA_SEQ_INDEX, 0x04);
	outb(VGA_SEQ_DATA, 0x02);
}

void vga_set_text_mode(void) {
	clear_all_planes();

	if (have_saved_text_regs) {
		write_regs(saved_text_regs);
	} else {
		write_regs(g_80x25_text);
	}
	reload_text_mode_font();

	/* Explicitly blank the visible text cells (space, light-grey-on-black)
	 * across the full addressable text window, for good measure. */
	{
		volatile uint16_t *vmem = (volatile uint16_t *)0xB8000;
		uint16_t blank = (uint16_t)' ' | ((uint16_t)0x07 << 8);
		for (int i = 0; i < 80 * 50; i++) {
			vmem[i] = blank;
		}
	}
}

void vga_set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
	outb(VGA_DAC_WRITE_INDEX, index);
	outb(VGA_DAC_DATA, r & 0x3F);
	outb(VGA_DAC_DATA, g & 0x3F);
	outb(VGA_DAC_DATA, b & 0x3F);
}
