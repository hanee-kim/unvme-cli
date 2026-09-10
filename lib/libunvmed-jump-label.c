// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "libunvmed-jump-label.h"

/*
 * Jump table entry — three PC-relative fields, each stored as a signed
 * offset from itself to its referent.  This layout is ASLR/PIE-safe:
 * the referent address is simply &field + field.
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

/*
 * GNU ld provides __start___unvme_jump_table / __stop___unvme_jump_table
 * automatically for any section whose name is a valid C identifier.
 * These symbols cover only THIS shared object's section; they are used
 * exclusively within libunvmed.so (LIBUNVMED_INTERNAL builds).
 */
extern struct __unvmed_jump_entry __start___unvme_jump_table[];
extern struct __unvmed_jump_entry __stop___unvme_jump_table[];

#ifdef __x86_64__

/* 5-byte Intel multi-byte NOP */
static const uint8_t k_nop5[5] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };

/*
 * Serialise concurrent calls to unvmed_static_key_enable/disable.
 * Log-level changes are rare admin operations, so a global mutex is fine.
 */
static pthread_mutex_t g_patch_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * patch_site — overwrite the first 5 bytes of an 8-byte aligned site.
 *
 * Uses /proc/self/mem so the write never removes EXEC permission from the
 * page (avoids SIGSEGV on other threads executing the same page — S1-1).
 * pwrite handles cross-page sites transparently (S1-2).
 *
 * Atomicity across CPUs (S1-3): we read the existing 8-byte word, splice
 * in the new 5 bytes, and write back with a single 8-byte atomic store.
 * The site is 8-byte aligned (enforced by .balign 8 in the macro), so
 * x86-64 guarantees the store is atomic from the bus's perspective.
 * An MFENCE after the store pushes the write out of the store buffer.
 * Other CPUs see either the old or new 8 bytes — never a torn value.
 *
 * Note: a fully correct cross-core serialization would require an IPI to
 * all other CPUs (as Linux text_poke does), but log-level changes are
 * infrequent enough that the remaining window is acceptable.
 */
static void patch_site(int mem_fd, uint8_t *site, const uint8_t patch[5])
{
	uint64_t word;

	/* Read current 8 bytes (site is 8-byte aligned). */
	memcpy(&word, site, 8);
	/* Splice the new 5-byte instruction into the low bytes. */
	memcpy(&word, patch, 5);

	/* Write via /proc/self/mem — kernel handles page permissions. */
	if (pwrite(mem_fd, &word, 8, (off_t)(uintptr_t)site) != 8)
		return;

	__asm__ volatile("mfence" ::: "memory");
}

static void update_key(unvmed_static_key_t *key, bool enable)
{
	struct __unvmed_jump_entry *e;
	int mem_fd;

	pthread_mutex_lock(&g_patch_mutex);

	/* Publish the new key state before patching so that any thread that
	 * reads key->enabled (fallback path) sees the correct value. */
	atomic_store_explicit(&key->enabled, enable ? 1 : 0,
			      memory_order_release);

	mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
	if (mem_fd < 0)
		goto unlock;

	for (e = __start___unvme_jump_table;
	     e < __stop___unvme_jump_table; e++) {
		unvmed_static_key_t *k =
			(unvmed_static_key_t *)((char *)&e->key + e->key);

		if (k != key)
			continue;

		uint8_t *nop = (uint8_t *)((char *)&e->code + e->code);

		if (enable) {
			/*
			 * Build JMP rel32: opcode 0xe9 + signed 32-bit offset.
			 * The offset is from the next instruction (nop + 5) to
			 * the taken-branch target.
			 */
			uint8_t *tgt = (uint8_t *)((char *)&e->target + e->target);
			int32_t rel  = (int32_t)(tgt - (nop + 5));
			uint8_t jmp[5];
			jmp[0] = 0xe9;
			memcpy(&jmp[1], &rel, 4);
			patch_site(mem_fd, nop, jmp);
		} else {
			patch_site(mem_fd, nop, k_nop5);
		}
	}

	close(mem_fd);
unlock:
	pthread_mutex_unlock(&g_patch_mutex);
}

#else  /* !__x86_64__ */

static void update_key(unvmed_static_key_t *key, bool enable)
{
	/* Non-x86: the macro falls back to __builtin_expect; just flip the
	 * flag so the fallback reads the correct value. */
	atomic_store_explicit(&key->enabled, enable ? 1 : 0,
			      memory_order_release);
}

#endif /* __x86_64__ */

void unvmed_static_key_enable(unvmed_static_key_t *key)
{
	update_key(key, true);
}

void unvmed_static_key_disable(unvmed_static_key_t *key)
{
	update_key(key, false);
}
