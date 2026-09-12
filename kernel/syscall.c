/* syscall.c - the kernel side of auroraOS's syscall ABI.
 *
 * Invoked via `int 0x80` (see boot.s's isr128 / isr_common_stub, and
 * idt.c's isr_handler() routing int_no==0x80 here instead of down the
 * fatal-exception path). This is what custom-compiled programs (see
 * asm.c) use to ask the kernel to do anything - print text, draw
 * pixels, read the keyboard, etc. - since they run with no C library
 * and no direct hardware access of their own.
 *
 * Calling convention (documented in full in docs/SYSCALLS.md):
 *   rax = syscall number (see the SYS_* constants in kernel.h)
 *   rdi, rsi, rdx, r10 = up to four arguments, in that order
 *   return value comes back in rax
 * Every syscall takes at most 4 plain integer/pointer arguments - none
 * of them pack multiple values into one register - so the ABI stays
 * simple enough to document and to write by hand in assembly. A couple
 * of rendering calls that conceptually need more than 4 values (e.g. a
 * rectangle's x/y/w/h/color) are split into a "set position" call
 * followed by the operation, rather than bit-packing arguments.
 */
#include "kernel.h"

/* SYS_GFX_RECT and SYS_GFX_LINE need more than 4 scalar values; rather
 * than pack two into one register (unfriendly to hand-written assembly
 * and to documenting the ABI), the caller stages the extra values with
 * SYS_GFX_SET_EXTRA first. This is the only piece of syscall state that
 * persists between calls. */
static uint64_t gfx_extra_a = 0;
static uint64_t gfx_extra_b = 0;

/* SYS_EXIT needs to unwind the running program immediately, not just
 * return a value to the instruction after `int80` the way every other
 * syscall does. Since the program is called directly (asm_run_trampoline
 * does `call *rdi` into it, no separate "process" context to switch
 * away from), the way to do that here is to make the `iretq` this
 * interrupt ends with resume execution at a single `ret` instruction
 * instead of back inside the program - which pops the trampoline's own
 * return address (still sitting on the stack from the original `call`)
 * and returns control to whoever invoked asm_run(), exactly as if the
 * program itself had executed `ret`. */
static uint8_t exit_stub[] = { 0xC3 }; /* ret */
uint64_t asm_last_exit_code = 0;

void syscall_dispatch(struct registers *regs) {
	uint64_t num = regs->rax;
	uint64_t a1 = regs->rdi;
	uint64_t a2 = regs->rsi;
	uint64_t a3 = regs->rdx;
	uint64_t a4 = regs->r10;
	uint64_t ret = 0;

	switch (num) {
		case SYS_EXIT:
			asm_last_exit_code = a1;
			regs->rip = (uint64_t)(uintptr_t)exit_stub;
			return; /* skip the regs->rax = ret write below: rax already holds the exit code, which is fine to leave as-is */

		case SYS_WRITE_CHAR:
			console_putchar((char)a1);
			console_present();
			break;

		case SYS_WRITE_STR: {
			const char *s = (const char *)(uintptr_t)a1;
			console_writestring(s);
			console_present();
			break;
		}

		case SYS_WRITE_INT:
			kprintf("%d", (int)(int64_t)a1);
			console_present();
			break;

		case SYS_READ_CHAR:
			ret = (uint64_t)(uint8_t)keyboard_getchar_blocking();
			break;

		case SYS_HAS_KEY:
			ret = keyboard_has_key() ? 1 : 0;
			break;

		case SYS_GFX_SET_EXTRA:
			gfx_extra_a = a1;
			gfx_extra_b = a2;
			break;

		case SYS_GFX_PIXEL:
			/* a1=x, a2=y, a3=color */
			gfx_putpixel((int)a1, (int)a2, (gfx_color_t)a3);
			break;

		case SYS_GFX_RECT:
			/* a1=x, a2=y, a3=color; w/h staged via SYS_GFX_SET_EXTRA(w,h) */
			gfx_fill_rect((int)a1, (int)a2, (int)gfx_extra_a, (int)gfx_extra_b, (gfx_color_t)a3);
			break;

		case SYS_GFX_LINE:
			/* a1=x0, a2=y0, a3=color; x1/y1 staged via SYS_GFX_SET_EXTRA(x1,y1) */
			gfx_draw_line((int)a1, (int)a2, (int)gfx_extra_a, (int)gfx_extra_b, (gfx_color_t)a3);
			break;

		case SYS_GFX_TEXT:
			/* a1=x, a2=y, a3=ptr to null-terminated string, a4=color */
			gfx_draw_string((int)a1, (int)a2, (const char *)(uintptr_t)a3, (gfx_color_t)a4);
			break;

		case SYS_GFX_FLIP:
			gfx_flip();
			break;

		case SYS_GFX_WIDTH:
			ret = (uint64_t)(int64_t)gfx_width();
			break;

		case SYS_GFX_HEIGHT:
			ret = (uint64_t)(int64_t)gfx_height();
			break;

		case SYS_GET_TICKS:
			ret = timer_get_ticks();
			break;

		case SYS_SLEEP_TICKS:
			timer_wait((uint32_t)a1);
			break;

		default:
			ret = (uint64_t)-1;
			break;
	}

	regs->rax = ret;
}
