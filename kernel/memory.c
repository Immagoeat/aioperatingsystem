#include "kernel.h"

struct multiboot_info {
	uint32_t flags;
	uint32_t mem_lower;
	uint32_t mem_upper;
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	uint32_t syms[4];
	uint32_t mmap_length;
	uint32_t mmap_addr;
	uint32_t drives_length;
	uint32_t drives_addr;
	uint32_t config_table;
	uint32_t boot_loader_name;
	uint32_t apm_table;
	uint32_t vbe_control_info;
	uint32_t vbe_mode_info;
	uint16_t vbe_mode;
	uint16_t vbe_interface_seg;
	uint16_t vbe_interface_off;
	uint16_t vbe_interface_len;
	uint64_t framebuffer_addr;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint8_t  framebuffer_bpp;
	uint8_t  framebuffer_type;
	uint8_t  color_info[6];
} __attribute__((packed));

#define MB_FLAG_FRAMEBUFFER (1 << 12)

struct mmap_entry {
	uint32_t size;
	uint64_t addr;
	uint64_t len;
	uint32_t type;
} __attribute__((packed));

static struct multiboot_info *mbi = 0;

void memory_init(uint32_t mb_info_addr) {
	mbi = (struct multiboot_info *)mb_info_addr;
}

uint32_t memory_total_kb(void) {
	if (!mbi) return 0;
	return mbi->mem_lower + mbi->mem_upper;
}

bool memory_get_framebuffer(struct fb_info *out) {
	if (!mbi || !(mbi->flags & MB_FLAG_FRAMEBUFFER)) return false;
	if (mbi->framebuffer_type != 1) return false; /* 1 = direct RGB */

	out->addr = mbi->framebuffer_addr;
	out->pitch = mbi->framebuffer_pitch;
	out->width = mbi->framebuffer_width;
	out->height = mbi->framebuffer_height;
	out->bpp = mbi->framebuffer_bpp;
	out->red_pos = mbi->color_info[0];
	out->red_size = mbi->color_info[1];
	out->green_pos = mbi->color_info[2];
	out->green_size = mbi->color_info[3];
	out->blue_pos = mbi->color_info[4];
	out->blue_size = mbi->color_info[5];

	/* Some GRUB/BIOS combinations report a red_size of 0 here, which is
	 * never actually correct for a 32/24bpp direct-color mode - it's a
	 * sign the color_info fields for this particular VBE mode weren't
	 * filled in reliably. When that happens, fall back to the standard
	 * (and near-universal in practice) little-endian XRGB8888 / RGB888
	 * layout: blue in the low byte, then green, then red. */
	if (out->red_size == 0) {
		out->blue_pos = 0;
		out->blue_size = 8;
		out->green_pos = 8;
		out->green_size = 8;
		out->red_pos = 16;
		out->red_size = 8;
	}

	return true;
}

void memory_print_map(void) {
	if (!mbi) {
		kprintf("No multiboot memory info available.\n");
		return;
	}

	kprintf("Lower memory: %u KB\n", mbi->mem_lower);
	kprintf("Upper memory: %u KB\n", mbi->mem_upper);
	kprintf("Total usable: %u KB (~%u MB)\n", memory_total_kb(), memory_total_kb() / 1024);

	if (mbi->flags & (1 << 6)) {
		kprintf("\nMemory map:\n");
		struct mmap_entry *entry = (struct mmap_entry *)mbi->mmap_addr;
		uint32_t end = mbi->mmap_addr + mbi->mmap_length;
		while ((uint32_t)entry < end) {
			kprintf("  addr=0x%x len=0x%x type=%d\n",
				(uint32_t)entry->addr, (uint32_t)entry->len, entry->type);
			entry = (struct mmap_entry *)((uint32_t)entry + entry->size + 4);
		}
	}
}
