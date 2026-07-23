/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_JUMP_LABEL_H
#define LIBUNVMED_JUMP_LABEL_H

#include <stdbool.h>

/*
 * Userspace static branch (jump label), mirroring the Linux kernel's
 * jump_label infrastructure.
 *
 * Key disabled  →  branch site holds a 5-byte NOP.
 *                  The CPU executes one harmless NOP and falls through.
 *                  Zero compare, zero prediction, zero overhead.
 *
 * Key enabled   →  the NOP is patched to a 5-byte JMP rel32.
 *                  The CPU always takes the jump — no misprediction,
 *                  still no compare instruction.
 *
 * Patching is done by unvmed_static_key_enable/disable at level-change
 * time (rare), not on every log call (hot path).
 */
typedef struct {
	int enabled;
} unvmed_static_key_t;

#define DEFINE_STATIC_KEY_FALSE(name)   unvmed_static_key_t name = { 0 }
#define DECLARE_STATIC_KEY_FALSE(name)  extern unvmed_static_key_t name

#ifdef __x86_64__

/*
 * asm goto emits:
 *   1. A 5-byte NOP at the call site (Intel's recommended multi-byte form).
 *   2. An entry in the __unvme_jump_table ELF section recording three
 *      PC-relative offsets: to the NOP, to the taken-branch target, and
 *      to the key variable.  All are relative to their own field address
 *      so they are ASLR/PIE safe with no runtime fixup needed.
 *
 * __label__ creates a GCC local label scoped to the statement expression,
 * so the macro may be used multiple times within the same function.
 */
# define unvmed_static_branch_unlikely(key)				\
  ({									\
	bool __ret;							\
	__label__ l_yes, l_out;						\
	asm goto(							\
		"1:\n\t"						\
		/* 5-byte NOP: 0f 1f 44 00 00 */			\
		".byte 0x0f, 0x1f, 0x44, 0x00, 0x00\n\t"		\
		".pushsection __unvme_jump_table, \"aw\"\n\t"		\
		".balign 8\n\t"						\
		/* &NOP - &entry.code  (int32) */			\
		".long 1b - .\n\t"					\
		/* &l_yes - &entry.target  (int32) */			\
		".long %l[l_yes] - .\n\t"				\
		/* &key - &entry.key  (int64) */			\
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

#else  /* !__x86_64__ — portable fallback */
# define unvmed_static_branch_unlikely(key) \
	__builtin_expect(!!(key)->enabled, 0)
#endif

void unvmed_static_key_enable(unvmed_static_key_t *key);
void unvmed_static_key_disable(unvmed_static_key_t *key);

#endif /* LIBUNVMED_JUMP_LABEL_H */
