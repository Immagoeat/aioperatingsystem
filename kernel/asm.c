/* asm.c - a small assembler for auroraOS's custom instruction set, plus
 * the flat-binary loader/runner. See docs/ASSEMBLY.md for the full
 * language reference this implements.
 *
 * Every encoded instruction form below was verified against real
 * GNU binutils (`as` + `objdump -d`) output for the exact operand
 * combination before being hand-transcribed here. This matters a great
 * deal for "no mistakes": x86-64 instruction encoding is easy to get
 * subtly wrong (REX prefix bits, ModRM reg/rm field order,
 * sign-extension of immediates), and there is no in-kernel disassembler
 * to catch an encoding bug after the fact - a wrong byte sequence just
 * silently runs as a different instruction.
 *
 * Deliberately NOT a general x86-64 assembler: no addressing-mode
 * combinatorics, no arbitrary immediate sizes, no relocations beyond
 * simple forward/backward label jumps within one file. Every
 * instruction form supported has exactly one fixed encoding shape,
 * which is what keeps this tractable to get right in one pass.
 */
#include "kernel.h"

#define MAX_LINES 512
#define MAX_LABELS 128
#define MAX_LINE_LEN 96
#define CODE_BUF_SIZE 65536

/* --- small local string helpers (not worth adding to the shared libc
 * just for this file) --- */

static void local_memmove(void *dst, const void *src, size_t n) {
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (d < s) {
		for (size_t i = 0; i < n; i++) d[i] = s[i];
	} else {
		for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
	}
}

static void local_strncpy(char *dst, const char *src, size_t n) {
	size_t i = 0;
	for (; i < n && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
}

static int32_t local_parse_int(const char *s) {
	bool neg = false;
	if (*s == '-') { neg = true; s++; }
	int32_t val = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		while (*s) {
			char c = *s;
			int digit;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
			else break;
			val = val * 16 + digit;
			s++;
		}
	} else {
		while (*s >= '0' && *s <= '9') {
			val = val * 10 + (*s - '0');
			s++;
		}
	}
	return neg ? -val : val;
}

static void trim(char *s) {
	while (*s == ' ' || *s == '\t') local_memmove(s, s + 1, strlen(s));
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

/* --- register name -> number (0=rax,1=rcx,2=rdx,3=rbx,4=rsp,5=rbp,6=rsi,7=rdi,8-15=r8-r15) --- */
static int reg_number(const char *name) {
	static const char *names[16] = {
		"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
		"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
	};
	for (int i = 0; i < 16; i++) {
		if (strcmp(name, names[i]) == 0) return i;
	}
	return -1;
}

/* --- memory operand parsing: "[reg]" or "[reg+N]" --- */
static bool parse_mem_operand(const char *arg, int *out_reg, int32_t *out_disp) {
	if (arg[0] != '[') return false;
	size_t len = strlen(arg);
	if (len < 3 || arg[len - 1] != ']') return false;

	char inner[32];
	if (len - 2 >= sizeof(inner)) return false;
	local_strncpy(inner, arg + 1, len - 2);

	char *plus = inner;
	while (*plus && *plus != '+') plus++;

	if (*plus == '+') {
		*plus = '\0';
		int reg = reg_number(inner);
		if (reg < 0) return false;
		*out_reg = reg;
		*out_disp = local_parse_int(plus + 1);
		return true;
	}

	int reg = reg_number(inner);
	if (reg < 0) return false;
	*out_reg = reg;
	*out_disp = 0;
	return true;
}

/* --- tokenizing one line: "OP ARG1, ARG2" --- */
struct line_tokens {
	char op[16];
	char arg1[32];
	char arg2[32];
	int arg_count;
};

static bool tokenize_line(const char *line, struct line_tokens *out) {
	memset(out, 0, sizeof(*out));

	char buf[MAX_LINE_LEN];
	local_strncpy(buf, line, sizeof(buf) - 1);

	/* strip a ';' or '#' comment */
	for (char *p = buf; *p; p++) {
		if (*p == ';' || *p == '#') { *p = '\0'; break; }
	}
	trim(buf);
	if (buf[0] == '\0') return false; /* blank/comment-only line */

	/* a lone "label:" line */
	size_t len = strlen(buf);
	if (buf[len - 1] == ':') {
		buf[len - 1] = '\0';
		strcpy(out->op, ":");
		strcpy(out->arg1, buf);
		out->arg_count = 1;
		return true;
	}

	char *saveptr;
	char *op = strtok_simple(buf, ' ', &saveptr);
	if (!op) return false;
	local_strncpy(out->op, op, sizeof(out->op) - 1);

	char *rest = strtok_simple(NULL, '\0', &saveptr);
	if (!rest) { out->arg_count = 0; return true; }
	trim(rest);
	if (rest[0] == '\0') { out->arg_count = 0; return true; }

	char *comma = rest;
	while (*comma && *comma != ',') comma++;
	if (*comma == ',') {
		*comma = '\0';
		local_strncpy(out->arg1, rest, sizeof(out->arg1) - 1);
		trim(out->arg1);
		char *arg2 = comma + 1;
		trim(arg2);
		local_strncpy(out->arg2, arg2, sizeof(out->arg2) - 1);
		out->arg_count = 2;
	} else {
		local_strncpy(out->arg1, rest, sizeof(out->arg1) - 1);
		trim(out->arg1);
		out->arg_count = 1;
	}
	return true;
}

/* --- assembler state and code emission --- */

struct label {
	char name[32];
	uint32_t offset; /* byte offset into the code buffer */
};

struct jump_fixup {
	uint32_t patch_offset;      /* offset of the 1-byte displacement in code[] */
	char label[32];
	uint32_t next_instr_offset; /* offset of the byte AFTER the jump, for relative calc */
	int line_number;
};

struct assembler_state {
	uint8_t code[CODE_BUF_SIZE];
	uint32_t code_len;

	struct label labels[MAX_LABELS];
	int label_count;

	struct jump_fixup fixups[MAX_LINES];
	int fixup_count;

	char error[128];
	int error_line;
	bool failed;
};

static void emit_byte(struct assembler_state *st, uint8_t b) {
	if (st->code_len < CODE_BUF_SIZE) st->code[st->code_len++] = b;
}

static void emit_imm32(struct assembler_state *st, int32_t v) {
	emit_byte(st, (uint8_t)(v & 0xFF));
	emit_byte(st, (uint8_t)((v >> 8) & 0xFF));
	emit_byte(st, (uint8_t)((v >> 16) & 0xFF));
	emit_byte(st, (uint8_t)((v >> 24) & 0xFF));
}

static void emit_imm64(struct assembler_state *st, int64_t v) {
	for (int i = 0; i < 8; i++) emit_byte(st, (uint8_t)((v >> (i * 8)) & 0xFF));
}

static void fail(struct assembler_state *st, int line_number, const char *msg) {
	if (st->failed) return; /* keep the first error */
	st->failed = true;
	st->error_line = line_number;
	local_strncpy(st->error, msg, sizeof(st->error) - 1);
}

/* --- instruction encoders ---
 *
 * Each of these mirrors one verified `objdump -d` output exactly (see
 * the file header). REX prefix bit meanings used throughout:
 *   0x48 = REX.W (64-bit operand size) - always set, every instruction
 *          here operates on full 64-bit registers
 *   0x44 = REX.R - set when the "reg" field operand is r8-r15
 *   0x41 = REX.B - set when the "rm" field / opcode-encoded operand is r8-r15
 * (0x48 | 0x44 | 0x41 = 0x4D when both apply, matching e.g. `add r8,r9` -> 4d 01 c8)
 */

/* mov reg, imm64 -> REX.W+B, B8+reg, imm64 (opcode 0xB8+reg is "MOV r64, imm64") */
static void enc_mov_reg_imm64(struct assembler_state *st, int dst, int64_t imm) {
	uint8_t rex = 0x48 | (dst >= 8 ? 0x01 : 0x00);
	emit_byte(st, rex);
	emit_byte(st, (uint8_t)(0xB8 + (dst & 7)));
	emit_imm64(st, imm);
}

/* mov dst, src (register-to-register) -> REX.W+R+B, 0x89, ModRM(11,src,dst) */
static void enc_mov_reg_reg(struct assembler_state *st, int dst, int src) {
	uint8_t rex = 0x48 | (src >= 8 ? 0x04 : 0x00) | (dst >= 8 ? 0x01 : 0x00);
	emit_byte(st, rex);
	emit_byte(st, 0x89);
	emit_byte(st, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* add/sub/cmp dst, src -> REX.W+R+B, opcode, ModRM(11,src,dst)
 * opcode: add=0x01, sub=0x29, cmp=0x39 (all "OP r/m64, r64" forms) */
static void enc_alu_reg_reg(struct assembler_state *st, uint8_t opcode, int dst, int src) {
	uint8_t rex = 0x48 | (src >= 8 ? 0x04 : 0x00) | (dst >= 8 ? 0x01 : 0x00);
	emit_byte(st, rex);
	emit_byte(st, opcode);
	emit_byte(st, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* add/sub/cmp dst, imm32 (sign-extended) -> REX.W+B, 0x81, ModRM(11,/ext,dst), imm32
 * /ext: add=0, sub=5, cmp=7 (the ModRM "reg" field selects the operation
 * for opcode 0x81, per the "group 1" encoding) */
static void enc_alu_reg_imm32(struct assembler_state *st, uint8_t ext, int dst, int32_t imm) {
	uint8_t rex = 0x48 | (dst >= 8 ? 0x01 : 0x00);
	emit_byte(st, rex);
	emit_byte(st, 0x81);
	emit_byte(st, (uint8_t)(0xC0 | (ext << 3) | (dst & 7)));
	emit_imm32(st, imm);
}

/* push/pop reg -> (REX.B if r8-r15), 0x50+reg / 0x58+reg */
static void enc_push_pop(struct assembler_state *st, uint8_t base_opcode, int reg) {
	if (reg >= 8) emit_byte(st, 0x41);
	emit_byte(st, (uint8_t)(base_opcode + (reg & 7)));
}

/* mov dst_reg, [src_reg+disp] -> REX.W+R+B, 0x8B, ModRM, [SIB if rsp/r12], disp32
 * mov [dst_reg+disp], src_reg -> REX.W+R+B, 0x89, ModRM, [SIB if rsp/r12], disp32
 * Always emits a disp32 (ModRM mod=10) rather than trying to use the
 * shorter disp8/no-disp forms, to keep the encoding logic uniform and
 * avoid an extra "which form fits" special case that could be gotten
 * wrong; costs a few extra bytes per memory access, which is
 * irrelevant at this program size.
 * rsp/r12 (register number 4, mod 8) as the base register requires a
 * SIB byte (0x24) even for a simple [reg+disp] form, per x86-64 rules -
 * handled explicitly since this ISA does allow using rsp as a pointer. */
static void enc_mov_mem(struct assembler_state *st, bool load, int reg_operand, int mem_reg, int32_t disp) {
	uint8_t rex = 0x48 | (reg_operand >= 8 ? 0x04 : 0x00) | (mem_reg >= 8 ? 0x01 : 0x00);
	emit_byte(st, rex);
	emit_byte(st, load ? 0x8B : 0x89);
	emit_byte(st, (uint8_t)(0x80 | ((reg_operand & 7) << 3) | (mem_reg & 7)));
	if ((mem_reg & 7) == 4) emit_byte(st, 0x24); /* SIB: scale=0,index=none(100),base=rsp/r12 */
	emit_imm32(st, disp);
}

/* --- two-pass assembly ---
 *
 * Pass 1: encode every instruction, recording each label's address as
 * it's defined and queuing a fixup for every jump (since a jump may
 * target a label not yet seen). Pass 2 (done inline via the fixups
 * list once both passes' worth of labels are known) patches every
 * short jump's 1-byte displacement. Short (8-bit) jumps only - the
 * programs this targets are small, and a displacement overflow is
 * caught explicitly rather than silently wrapping. */

static int find_label(struct assembler_state *st, const char *name) {
	for (int i = 0; i < st->label_count; i++) {
		if (strcmp(st->labels[i].name, name) == 0) return i;
	}
	return -1;
}

static void add_label(struct assembler_state *st, const char *name, uint32_t offset, int line_number) {
	if (find_label(st, name) >= 0) {
		fail(st, line_number, "duplicate label");
		return;
	}
	if (st->label_count >= MAX_LABELS) {
		fail(st, line_number, "too many labels");
		return;
	}
	local_strncpy(st->labels[st->label_count].name, name, sizeof(st->labels[0].name) - 1);
	st->labels[st->label_count].offset = offset;
	st->label_count++;
}

/* condition-code jump opcodes: 0x74=je, 0x75=jne, 0x7C=jl, 0x7D=jge,
 * 0x7F=jg, 0x7E=jle, 0xEB=jmp (unconditional) - all short (8-bit
 * displacement) forms, matching the verified objdump output. */
static bool jump_opcode_for(const char *mnemonic, uint8_t *out_opcode) {
	struct { const char *name; uint8_t opcode; } table[] = {
		{"jmp", 0xEB}, {"je", 0x74}, {"jne", 0x75},
		{"jl", 0x7C}, {"jge", 0x7D}, {"jg", 0x7F}, {"jle", 0x7E},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (strcmp(mnemonic, table[i].name) == 0) { *out_opcode = table[i].opcode; return true; }
	}
	return false;
}

bool asm_assemble(const char *source, uint8_t *out_code, uint32_t out_capacity, uint32_t *out_len, char *out_error, int *out_error_line) {
	static struct assembler_state st;
	memset(&st, 0, sizeof(st));

	/* split source into lines in place isn't safe (source may be
	 * read-only / const); copy line-by-line instead */
	int line_number = 0;
	const char *p = source;

	while (*p) {
		char line_buf[MAX_LINE_LEN];
		int i = 0;
		while (*p && *p != '\n' && i < MAX_LINE_LEN - 1) line_buf[i++] = *p++;
		line_buf[i] = '\0';
		if (*p == '\n') p++;
		line_number++;

		struct line_tokens tok;
		if (!tokenize_line(line_buf, &tok)) continue;

		if (strcmp(tok.op, ":") == 0) {
			add_label(&st, tok.arg1, st.code_len, line_number);
			continue;
		}

		uint8_t jump_opcode;
		if (jump_opcode_for(tok.op, &jump_opcode)) {
			if (tok.arg_count != 1) { fail(&st, line_number, "jump needs one label argument"); continue; }
			emit_byte(&st, jump_opcode);
			uint32_t patch_offset = st.code_len;
			emit_byte(&st, 0x00); /* placeholder displacement */
			if (st.fixup_count < MAX_LINES) {
				struct jump_fixup *fx = &st.fixups[st.fixup_count++];
				fx->patch_offset = patch_offset;
				local_strncpy(fx->label, tok.arg1, sizeof(fx->label) - 1);
				fx->next_instr_offset = st.code_len;
				fx->line_number = line_number;
			}
			continue;
		}

		if (strcmp(tok.op, "mov") == 0) {
			if (tok.arg_count != 2) { fail(&st, line_number, "mov needs two arguments"); continue; }

			int mem_reg; int32_t disp;
			if (parse_mem_operand(tok.arg1, &mem_reg, &disp)) {
				int src = reg_number(tok.arg2);
				if (src < 0) { fail(&st, line_number, "mov [mem], X needs a register source"); continue; }
				enc_mov_mem(&st, false, src, mem_reg, disp);
				continue;
			}
			if (parse_mem_operand(tok.arg2, &mem_reg, &disp)) {
				int dst = reg_number(tok.arg1);
				if (dst < 0) { fail(&st, line_number, "mov X, [mem] needs a register destination"); continue; }
				enc_mov_mem(&st, true, dst, mem_reg, disp);
				continue;
			}

			int dst = reg_number(tok.arg1);
			if (dst < 0) { fail(&st, line_number, "unknown destination register"); continue; }

			int src = reg_number(tok.arg2);
			if (src >= 0) {
				enc_mov_reg_reg(&st, dst, src);
			} else {
				int64_t imm = local_parse_int(tok.arg2);
				enc_mov_reg_imm64(&st, dst, imm);
			}
			continue;
		}

		if (strcmp(tok.op, "add") == 0 || strcmp(tok.op, "sub") == 0 || strcmp(tok.op, "cmp") == 0) {
			if (tok.arg_count != 2) { fail(&st, line_number, "expected two arguments"); continue; }
			int dst = reg_number(tok.arg1);
			if (dst < 0) { fail(&st, line_number, "unknown destination register"); continue; }

			int src = reg_number(tok.arg2);
			if (src >= 0) {
				uint8_t opcode = strcmp(tok.op, "add") == 0 ? 0x01 : (strcmp(tok.op, "sub") == 0 ? 0x29 : 0x39);
				enc_alu_reg_reg(&st, opcode, dst, src);
			} else {
				int32_t imm = local_parse_int(tok.arg2);
				uint8_t ext = strcmp(tok.op, "add") == 0 ? 0 : (strcmp(tok.op, "sub") == 0 ? 5 : 7);
				enc_alu_reg_imm32(&st, ext, dst, imm);
			}
			continue;
		}

		if (strcmp(tok.op, "push") == 0) {
			int reg = reg_number(tok.arg1);
			if (reg < 0) { fail(&st, line_number, "unknown register"); continue; }
			enc_push_pop(&st, 0x50, reg);
			continue;
		}
		if (strcmp(tok.op, "pop") == 0) {
			int reg = reg_number(tok.arg1);
			if (reg < 0) { fail(&st, line_number, "unknown register"); continue; }
			enc_push_pop(&st, 0x58, reg);
			continue;
		}

		if (strcmp(tok.op, "int80") == 0 || strcmp(tok.op, "syscall") == 0) {
			emit_byte(&st, 0xCD);
			emit_byte(&st, 0x80);
			continue;
		}

		if (strcmp(tok.op, "ret") == 0) {
			emit_byte(&st, 0xC3);
			continue;
		}

		fail(&st, line_number, "unknown instruction");
	}

	/* pass 2: patch jump displacements now that every label is known */
	if (!st.failed) {
		for (int i = 0; i < st.fixup_count; i++) {
			struct jump_fixup *fx = &st.fixups[i];
			int label_idx = find_label(&st, fx->label);
			if (label_idx < 0) {
				fail(&st, fx->line_number, "undefined label");
				break;
			}
			int64_t rel = (int64_t)st.labels[label_idx].offset - (int64_t)fx->next_instr_offset;
			if (rel < -128 || rel > 127) {
				fail(&st, fx->line_number, "jump target too far (short jumps only, max +-127 bytes)");
				break;
			}
			st.code[fx->patch_offset] = (uint8_t)(int8_t)rel;
		}
	}

	if (st.failed) {
		if (out_error) local_strncpy(out_error, st.error, 127);
		if (out_error_line) *out_error_line = st.error_line;
		return false;
	}

	if (st.code_len > out_capacity) {
		if (out_error) local_strncpy(out_error, "program too large", 127);
		if (out_error_line) *out_error_line = 0;
		return false;
	}

	memcpy(out_code, st.code, st.code_len);
	if (out_len) *out_len = st.code_len;
	return true;
}

/* --- running an assembled program ---
 *
 * The assembled bytes are real x86-64 machine code, executed directly
 * (no interpreter, no sandboxing) - matching the "flat binary, zero
 * isolation" design: a buggy program can genuinely crash or corrupt
 * the running kernel, the same way a bug in kernel code itself could.
 * Entry is a bare `call` to the start of the buffer; the program is
 * expected to end with a syscall (SYS_EXIT via `mov rax,0` / `int80`)
 * or a `ret`, at which point control returns here normally. */
extern void asm_run_trampoline(void *code);

uint64_t asm_run(const uint8_t *code, uint32_t len) {
	/* Executable data must live in a buffer the CPU will actually
	 * fetch instructions from; since this kernel's page tables identity
	 * map everything as present+writable (no NX bit set anywhere, see
	 * boot.s), any normal buffer is already executable - there's no
	 * separate "mark this memory executable" step needed here. */
	static uint8_t exec_buf[CODE_BUF_SIZE];
	if (len > sizeof(exec_buf)) return (uint64_t)-1;
	memcpy(exec_buf, code, len);

	asm_last_exit_code = 0;
	asm_run_trampoline(exec_buf);
	return asm_last_exit_code;
}
