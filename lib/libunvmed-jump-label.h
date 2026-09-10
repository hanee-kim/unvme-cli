/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_JUMP_LABEL_H
#define LIBUNVMED_JUMP_LABEL_H

#include <stdbool.h>
#include <stdatomic.h>

/*
 * Userspace static branch (jump label), mirroring the Linux kernel's
 * jump_label infrastructure.
 *
 * Key disabled  →  branch site holds a 5-byte NOP followed by a 3-byte NOP
 *                  (8 bytes total, 8-byte aligned).  The CPU executes the
 *                  NOPs and falls through.  Zero compare, zero prediction.
 *
 * Key enabled   →  the 5-byte NOP is patched to a 5-byte JMP rel32 via an
 *                  8-byte atomic store (the trailing 3 NOP bytes are
 *                  unchanged dead code).  The CPU always takes the jump —
 *                  no misprediction, still no compare instruction.
 *
 * Patching is done by unvmed_static_key_enable/disable at level-change
 * time (rare), not on every log call (hot path).
 *
 * LIBUNVMED_INTERNAL:
 *   The asm goto / jump-table path is compiled only inside libunvmed.so
 *   (where -DLIBUNVMED_INTERNAL is set by meson).  External translation
 *   units (src/, fio plugin) fall back to __builtin_expect so they are
 *   not affected by the ELF-section-scope limitation of __start/__stop
 *   linker symbols.
 */
typedef struct {
	_Atomic int enabled;  /* 0 = key off, 1 = key on */
} unvmed_static_key_t;

/*
 * visibility("protected") makes the symbol non-preemptible in PIC builds
 * while keeping it visible to the dynamic linker.  Without this the
 * assembler "i" constraint (compile-time address) fails under -fPIC because
 * the symbol address is not a link-time constant.
 */
#define DEFINE_STATIC_KEY_FALSE(name)					\
	__attribute__((visibility("protected")))			\
	unvmed_static_key_t name = { 0 }

#define DECLARE_STATIC_KEY_FALSE(name)					\
	extern __attribute__((visibility("protected")))			\
	unvmed_static_key_t name

#if defined(__x86_64__) && defined(LIBUNVMED_INTERNAL)

/*
 * asm goto emits (per call site):
 *   1. An 8-byte aligned block: 5-byte NOP + 3-byte NOP.
 *   2. An entry in the __unvme_jump_table ELF section recording three
 *      PC-relative offsets: to the NOP, to the taken-branch target, and
 *      to the key variable.  All offsets are relative to their own field
 *      address → ASLR/PIE safe with no runtime fixup.
 *
 * The 8-byte aligned block lets patch_site() replace the first 5 bytes
 * with a single 8-byte atomic store, preventing other CPUs from ever
 * fetching a partially-written instruction.
 *
 * __label__ declarations must appear before any other declarations in a
 * GCC statement-expression block.
 */
# define unvmed_static_branch_unlikely(key)				\
  ({									\
	__label__ l_yes, l_out;						\
	bool __ret;							\
	asm goto(							\
		".balign 8\n\t"						\
		"1:\n\t"						\
		/* 5-byte NOP: 0f 1f 44 00 00 */			\
		".byte 0x0f, 0x1f, 0x44, 0x00, 0x00\n\t"		\
		/* 3-byte NOP padding for 8-byte atomic store */	\
		".byte 0x0f, 0x1f, 0x00\n\t"				\
		".pushsection __unvme_jump_table, \"aw\"\n\t"		\
		".balign 8\n\t"						\
		/* &NOP - &entry.code  (int32) */			\
		".long 1b - .\n\t"					\
		/* &l_yes - &entry.target  (int32) */			\
		".long %l[l_yes] - .\n\t"				\
		/* &key - &entry.key  (int64, protected so non-preemptible) */\
		".quad %c0 - .\n\t"					\
		".popsection\n\t"					\
		: : "i" (key) : : l_yes);				\
	__ret = false;							\
	goto l_out;							\
l_yes:									\
	__ret = true;							\
l_out:									\
	__ret;								\
  })

#else  /* !(__x86_64__ && LIBUNVMED_INTERNAL) — portable fallback */
# define unvmed_static_branch_unlikely(key) \
	__builtin_expect(						\
		!!atomic_load_explicit(&(key)->enabled,			\
				       memory_order_acquire), 0)
#endif

void unvmed_static_key_enable(unvmed_static_key_t *key);
void unvmed_static_key_disable(unvmed_static_key_t *key);

#endif /* LIBUNVMED_JUMP_LABEL_H */
