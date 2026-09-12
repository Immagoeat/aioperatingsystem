#include "kernel.h"

/* In long mode the GDT is mostly vestigial: segment base/limit are
 * ignored for code and data segments (paging does all real address
 * translation), except for the code segment's L-bit (marks it as a
 * 64-bit segment) and D-bit. We still install our own GDT here rather
 * than keep running on the small bootstrap one boot.s used to enter
 * long mode, so the kernel owns and can later extend it (e.g. a TSS
 * descriptor, user-mode segments) instead of pointing at boot.s's
 * throwaway one forever. */
struct gdt_entry {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_middle;
	uint8_t access;
	uint8_t granularity;
	uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr gp;

extern void gdt_flush(struct gdt_ptr *);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
	gdt[num].base_low = (base & 0xFFFF);
	gdt[num].base_middle = (base >> 16) & 0xFF;
	gdt[num].base_high = (base >> 24) & 0xFF;

	gdt[num].limit_low = (limit & 0xFFFF);
	gdt[num].granularity = (limit >> 16) & 0x0F;

	gdt[num].granularity |= gran & 0xF0;
	gdt[num].access = access;
}

void gdt_install(void) {
	gp.limit = (sizeof(struct gdt_entry) * 5) - 1;
	gp.base = (uint64_t)(uintptr_t)&gdt;

	gdt_set_gate(0, 0, 0, 0, 0);                /* null */
	gdt_set_gate(1, 0, 0, 0x9A, 0x20);           /* 64-bit code: L-bit set, base/limit ignored */
	gdt_set_gate(2, 0, 0, 0x92, 0x00);           /* 64-bit data */
	gdt_set_gate(3, 0, 0, 0xFA, 0x20);           /* user code */
	gdt_set_gate(4, 0, 0, 0xF2, 0x00);           /* user data */

	gdt_flush(&gp);
}
