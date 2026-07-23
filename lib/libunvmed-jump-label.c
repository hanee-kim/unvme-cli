// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libunvmed-jump-label.h"

/*
 * Jump table entry — three PC-relative fields, each stored as a signed
 * offset from itself to its referent.  This layout means no fixup is
 * needed for PIE/ASLR: the referent address is simply &field + field.
 *
 *   code   (int32_t): &NOP_instruction - &entry->code
 *   target (int32_t): &l_yes_label     - &entry->target
 *   key    (int64_t): &key_variable    - &entry->key
 */
struct __unvmed_jump_entry {
	int32_t code;
	int32_t target;
	int64_t key;
};

/* GNU ld provides these symbols automatically for sections whose names
 * are valid C identifiers.  __unvme_jump_table qualifies. */
extern struct __unvmed_jump_entry __start___unvme_jump_table[];
extern struct __unvmed_jump_entry __stop___unvme_jump_table[];

#ifdef __x86_64__

/* 5-byte Intel multi-byte NOP */
static const uint8_t k_nop5[5] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };

/*
 * Overwrite 5 bytes at @site with @patch using a W^X mprotect dance.
 * The mprotect syscall acts as a full serialising barrier on Linux/x86,
 * so no explicit CPUID/MFENCE is required after the write.
 */
static void patch_site(uint8_t *site, const uint8_t patch[5])
{
	long page_size = sysconf(_SC_PAGESIZE);
	void *page = (void *)((uintptr_t)site & ~(uintptr_t)(page_size - 1));

	mprotect(page, page_size, PROT_READ | PROT_WRITE);
	memcpy(site, patch, 5);
	mprotect(page, page_size, PROT_READ | PROT_EXEC);
}

static void update_key(unvmed_static_key_t *key, bool enable)
{
	struct __unvmed_jump_entry *e;

	key->enabled = enable ? 1 : 0;

	for (e = __start___unvme_jump_table;
	     e < __stop___unvme_jump_table; e++) {
		unvmed_static_key_t *k =
			(unvmed_static_key_t *)((char *)&e->key + e->key);

		if (k != key)
			continue;

		uint8_t *nop = (uint8_t *)((char *)&e->code + e->code);

		if (enable) {
			/* Build JMP rel32: opcode 0xe9 + 32-bit relative offset.
			 * The offset is relative to the instruction after JMP,
			 * i.e. target - (nop + 5). */
			uint8_t *tgt = (uint8_t *)((char *)&e->target + e->target);
			int32_t rel  = (int32_t)(tgt - (nop + 5));
			uint8_t jmp[5];
			jmp[0] = 0xe9;
			memcpy(&jmp[1], &rel, 4);
			patch_site(nop, jmp);
		} else {
			patch_site(nop, k_nop5);
		}
	}
}

#else  /* !__x86_64__ */

static void update_key(unvmed_static_key_t *key, bool enable)
{
	/* On non-x86 the macro falls back to __builtin_expect; just flip the
	 * flag so that fallback reads the right value. */
	key->enabled = enable ? 1 : 0;
}

#endif

void unvmed_static_key_enable(unvmed_static_key_t *key)
{
	update_key(key, true);
}

void unvmed_static_key_disable(unvmed_static_key_t *key)
{
	update_key(key, false);
}
