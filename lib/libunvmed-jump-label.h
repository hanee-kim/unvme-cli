/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_JUMP_LABEL_H
#define LIBUNVMED_JUMP_LABEL_H

#include <stdbool.h>
#include <stdatomic.h>

/*
 * Userspace static branch (jump label), mirroring the Linux kernel's
 * jump_label infrastructure.
 *
 * The branch site always holds a 5-byte instruction whose 4-byte tail is the
 * rel32 displacement to the taken-branch target, baked in by the assembler.
 * Only the opcode byte is ever rewritten:
 *
 *   Key disabled  →  A9 id  =  TEST EAX, imm32
 *                    Five bytes, no branch, no memory access; the immediate
 *                    is the (ignored) displacement.  Falls through.
 *
 *   Key enabled   →  E9 id  =  JMP rel32
 *                    Same five bytes, now taken.  No compare, no
 *                    misprediction.
 *
 * Why a one-byte patch rather than swapping a 5-byte NOP for a 5-byte JMP:
 * rewriting several bytes of a live instruction is cross-modifying code.  A
 * core that is fetching the site concurrently may pair the new opcode with
 * the stale displacement still in its prefetch buffer and branch to a wild
 * address — an aligned 8-byte store does not prevent this, because the
 * hazard is in instruction fetch, not in store atomicity.  Rewriting a
 * single byte removes the hazard by construction: both encodings are exactly
 * five bytes and both are valid, so any core observes either the whole old
 * instruction or the whole new one.
 *
 * TEST writes EFLAGS, hence the "cc" clobber below.
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
 *   1. The 5-byte branch site: opcode byte + rel32 to l_yes.  The
 *      displacement is resolved by the assembler/linker, so the runtime
 *      never computes or writes it — and a target out of rel32 range is a
 *      link error rather than a silently truncated jump.
 *   2. An entry in the __unvme_jump_table ELF section holding two
 *      PC-relative offsets: to the site and to the key.  Each offset is
 *      relative to its own field address → ASLR/PIE safe, no runtime fixup.
 *
 * __label__ declarations must appear before any other declarations in a
 * GCC statement-expression block.
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
