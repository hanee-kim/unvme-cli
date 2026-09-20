/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_JUMP_LABEL_H
#define LIBUNVMED_JUMP_LABEL_H

#include <stdbool.h>
#include <stdatomic.h>

/*
 * Userspace static branch, after the kernel's jump_label: a disabled branch
 * site executes nothing at all rather than loading and testing a flag.
 *
 * The site always holds a 5-byte instruction whose 4-byte tail is the rel32
 * displacement to the taken-branch target, baked in by the assembler.  Only
 * the opcode byte is rewritten:
 *
 *   disabled  A9 id   TEST EAX, imm32   no branch, immediate ignored
 *   enabled   E9 id   JMP  rel32
 *
 * Both encodings are exactly five bytes and both are valid, so a core
 * fetching the site while it is patched decodes either the whole old
 * instruction or the whole new one.  Rewriting more than one byte would not
 * be safe: the hazard in cross-modifying code is a torn instruction, and no
 * store width prevents it because the tear happens in instruction fetch.
 *
 * Patching happens on a log-level change, not on the logging path.
 */
typedef struct {
	_Atomic int enabled;  /* read only by the portable fallback below */
} unvmed_static_key_t;

/*
 * protected visibility keeps the symbol non-preemptible under -fPIC, which
 * the "i" constraint below needs: a preemptible symbol has no link-time
 * constant address.
 */
#define DEFINE_STATIC_KEY_FALSE(name)					\
	__attribute__((visibility("protected")))			\
	unvmed_static_key_t name = { 0 }

#define DECLARE_STATIC_KEY_FALSE(name)					\
	extern __attribute__((visibility("protected")))			\
	unvmed_static_key_t name

#if defined(__x86_64__) && defined(LIBUNVMED_INTERNAL)

/*
 * Emits the branch site plus an __unvme_jump_table entry recording where the
 * site and the key are.  Both offsets are relative to their own field, so no
 * runtime relocation is needed under ASLR or PIE.
 *
 * The displacement is resolved by the assembler, so a target out of rel32
 * range is a link error rather than a silently truncated jump.
 *
 * __label__ must precede any other declaration in a statement expression.
 */
# define unvmed_static_branch_unlikely(key)				\
  ({									\
	__label__ l_yes, l_out;						\
	bool __ret;							\
	asm goto(							\
		"1:\n\t"						\
		/* A9 id = TEST EAX, imm32 — patched to E9 (JMP rel32) */\
		".byte 0xa9\n\t"					\
		".long %l[l_yes] - (1b + 5)\n\t"			\
		".pushsection __unvme_jump_table, \"aw\"\n\t"		\
		".balign 8\n\t"						\
		/* &site - &entry.code */				\
		".quad 1b - .\n\t"					\
		/* &key - &entry.key (protected → non-preemptible) */	\
		".quad %c0 - .\n\t"					\
		".popsection\n\t"					\
		: : "i" (key) : "cc" : l_yes);				\
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
