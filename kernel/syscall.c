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

/* Yielding (fiber.c's fiber_yield()) from INSIDE a syscall handler is
 * a genuinely different situation from a fiber calling it from its own
 * normal code: this whole function is running nested inside the int80
 * ISR (isr128/isr_common_stub in boot.s), which entered with `cli` and
 * only runs `sti` right before its own `iretq` at the very end - so as
 * far as the CPU is concerned, interrupts are still OFF for as long as
 * this ISR call is "in progress". fiber_yield() doesn't know or care
 * about any of that (it just swaps %rsp - see fiber.c/boot.s's
 * fiber_switch), so a plain fiber_yield() here would suspend this ISR
 * call mid-flight with interrupts disabled and not re-enable them
 * until this exact fiber is resumed and finishes the syscall - which
 * could be many WM frames away, freezing the keyboard/timer/mouse the
 * entire time. Bracketing the yield with sti/cli restores the
 * invariant isr_common_stub expects (interrupts off on the way in,
 * back on on the way out) around the part that actually leaves this
 * stack frame suspended, without changing isr_common_stub itself. */
static void syscall_yield(void) {
	__asm__ volatile("sti");
	fiber_yield();
	__asm__ volatile("cli");
}

/* When the calling code is a loaded windowed app's own fiber (see
 * apploader.c's file comment), every SYS_GFX_* call below needs to
 * land inside that app's own window instead of the full screen - a
 * loaded app never gets to touch the real framebuffer coordinates
 * directly, the same way a real windowing system clips client
 * drawing to its own window. This looks up that window's current
 * content-area rect (apploader.c doesn't have access to wm.c's window
 * list itself, so wm_window_rect() is the one call across that
 * boundary - see kernel.h's doc comment on it); returns false (nothing
 * to clip to, caller should skip drawing) only in the should-never-
 * happen case of a loaded app's window having been closed out from
 * under it mid-syscall. */
static bool gfx_clip_active;
static int gfx_clip_x0, gfx_clip_y0, gfx_clip_x1, gfx_clip_y1;

static bool gfx_apply_clip(void) {
	gfx_clip_active = false;
	int window_id = apploader_current_window_id();
	if (window_id < 0) return true; /* not a windowed app - draw straight to the real screen, unclipped, same as before apploader.c existed */

	int wx, wy, ww, wh;
	if (!wm_window_rect(window_id, &wx, &wy, &ww, &wh)) return false;

	gfx_clip_active = true;
	gfx_clip_x0 = wx; gfx_clip_y0 = wy;
	gfx_clip_x1 = wx + ww; gfx_clip_y1 = wy + wh;
	return true;
}

/* Translates one app-relative coordinate pair into real screen
 * coordinates (just an offset - actual clamping happens per-primitive
 * below, since a point, a rect, and a line each need slightly
 * different bounds handling). */
static void gfx_clip_offset(int *x, int *y) {
	if (!gfx_clip_active) return;
	*x += gfx_clip_x0;
	*y += gfx_clip_y0;
}

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

		case SYS_GFX_PIXEL: {
			/* a1=x, a2=y, a3=color */
			if (!gfx_apply_clip()) break;
			int x = (int)a1, y = (int)a2;
			gfx_clip_offset(&x, &y);
			if (gfx_clip_active && (x < gfx_clip_x0 || x >= gfx_clip_x1 || y < gfx_clip_y0 || y >= gfx_clip_y1)) break;
			gfx_putpixel(x, y, (gfx_color_t)a3);
			break;
		}

		case SYS_GFX_RECT: {
			/* a1=x, a2=y, a3=color; w/h staged via SYS_GFX_SET_EXTRA(w,h) */
			if (!gfx_apply_clip()) break;
			int x = (int)a1, y = (int)a2, w = (int)gfx_extra_a, h = (int)gfx_extra_b;
			gfx_clip_offset(&x, &y);
			if (gfx_clip_active) {
				/* clamp the rect into the window's bounds rather than
				 * just its origin - gfx_fill_rect() already clips to
				 * the real screen edges the same way, this is that
				 * same idea one level in */
				if (x < gfx_clip_x0) { w -= (gfx_clip_x0 - x); x = gfx_clip_x0; }
				if (y < gfx_clip_y0) { h -= (gfx_clip_y0 - y); y = gfx_clip_y0; }
				if (x + w > gfx_clip_x1) w = gfx_clip_x1 - x;
				if (y + h > gfx_clip_y1) h = gfx_clip_y1 - y;
				if (w <= 0 || h <= 0) break;
			}
			gfx_fill_rect(x, y, w, h, (gfx_color_t)a3);
			break;
		}

		case SYS_GFX_LINE: {
			/* a1=x0, a2=y0, a3=color; x1/y1 staged via SYS_GFX_SET_EXTRA(x1,y1) */
			if (!gfx_apply_clip()) break;
			int x0 = (int)a1, y0 = (int)a2, x1 = (int)gfx_extra_a, y1 = (int)gfx_extra_b;
			gfx_clip_offset(&x0, &y0);
			gfx_clip_offset(&x1, &y1);
			/* No general line-clipping (Cohen-Sutherland etc.) here -
			 * gfx_draw_line() already clips each pixel it plots to the
			 * real screen bounds (see gfx_putpixel), so a line that
			 * runs past the window's edge just draws past it onto
			 * whatever's underneath rather than crashing; an exact
			 * clip would need real geometry this ISA's programs are
			 * unlikely to be sophisticated enough to notice missing. */
			gfx_draw_line(x0, y0, x1, y1, (gfx_color_t)a3);
			break;
		}

		case SYS_GFX_TEXT: {
			/* a1=x, a2=y, a3=ptr to null-terminated string, a4=color */
			if (!gfx_apply_clip()) break;
			int x = (int)a1, y = (int)a2;
			gfx_clip_offset(&x, &y);
			gfx_draw_string(x, y, (const char *)(uintptr_t)a3, (gfx_color_t)a4);
			break;
		}

		case SYS_GFX_FLIP:
			/* A windowed app doesn't own the real screen - wm.c's own
			 * frame loop flips once after compositing every window, so
			 * a loaded app flipping here itself would either do
			 * nothing useful or show a half-composited frame. Only a
			 * plain `run` program (no window, drawing straight to the
			 * real screen) actually needs this. */
			if (apploader_current_window_id() < 0) gfx_flip();
			break;

		case SYS_GFX_WIDTH:
			if (apploader_current_window_id() >= 0) {
				int wx, wy, ww, wh;
				ret = wm_window_rect(apploader_current_window_id(), &wx, &wy, &ww, &wh) ? (uint64_t)(int64_t)ww : 0;
			} else {
				ret = (uint64_t)(int64_t)gfx_width();
			}
			break;

		case SYS_GFX_HEIGHT:
			if (apploader_current_window_id() >= 0) {
				int wx, wy, ww, wh;
				ret = wm_window_rect(apploader_current_window_id(), &wx, &wy, &ww, &wh) ? (uint64_t)(int64_t)wh : 0;
			} else {
				ret = (uint64_t)(int64_t)gfx_height();
			}
			break;

		case SYS_GET_TICKS:
			ret = timer_get_ticks();
			break;

		case SYS_SLEEP_TICKS:
			timer_wait((uint32_t)a1);
			break;

		case SYS_YIELD:
			syscall_yield();
			break;

		case SYS_WM_CREATE_WINDOW:
			ret = apploader_syscall_create_window((int)a1, (int)a2) ? 1 : 0;
			syscall_yield(); /* see apploader.c's file comment on apploader_load()'s handshake - this is the point a freshly-loaded app hands control back so its window can actually be created */
			break;

		case SYS_WM_POLL_EVENT:
			ret = (uint64_t)(int64_t)apploader_syscall_poll_event();
			break;

		case SYS_WM_EVENT_KEY:
			ret = (uint64_t)(int64_t)apploader_syscall_event_key();
			break;

		case SYS_WM_EVENT_X:
			ret = (uint64_t)(int64_t)apploader_syscall_event_x();
			break;

		case SYS_WM_EVENT_Y:
			ret = (uint64_t)(int64_t)apploader_syscall_event_y();
			break;

		case SYS_WM_EVENT_DELTA:
			ret = (uint64_t)(int64_t)apploader_syscall_event_delta();
			break;

		default:
			ret = (uint64_t)-1;
			break;
	}

	regs->rax = ret;
}
