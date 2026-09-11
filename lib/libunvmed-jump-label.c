// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/membarrier.h>
#endif

#include "libunvmed-jump-label.h"

/*
 * Jump table entry — two PC-relative fields, each stored as a signed offset
 * from itself to its referent.  This layout is ASLR/PIE-safe: the referent
 * address is simply &field + field.
 *
 *   code (int64_t): &branch_site  - &entry->code
 *   key  (int64_t): &key_variable - &entry->key
 *
 * The branch target is not recorded: the rel32 displacement is baked into
 * the site by the assembler and never rewritten at runtime.
 */
struct __unvmed_jump_entry {
	int64_t code;
	int64_t key;
};

/*
 * GNU ld provides __start___unvme_jump_table / __stop___unvme_jump_table
 * automatically for any section whose name is a valid C identifier.
 * These symbols cover only THIS shared object's section; they are used
 * exclusively within libunvmed.so (LIBUNVMED_INTERNAL builds).
 *
 * Weak, because the linker only synthesises them if the section exists at
 * all.  A build in which every static-branch site happened to be compiled
 * out would otherwise fail to link with an undefined reference from this
 * file — making the library's linkability depend on some unrelated
 * translation unit happening to use a key.  Weak symbols resolve to NULL
 * instead, and the loop below then simply finds no entries.
 */
extern struct __unvmed_jump_entry __start___unvme_jump_table[]
	__attribute__((weak));
extern struct __unvmed_jump_entry __stop___unvme_jump_table[]
	__attribute__((weak));

#ifdef __x86_64__

/*
 * The two interchangeable opcodes for the 5-byte branch site.  Both consume
 * the same 4-byte displacement that follows, so swapping one for the other
 * never changes the instruction length.
 */
#define OP_JMP_REL32	0xe9	/* JMP rel32     — key enabled  */
#define OP_TEST_EAX	0xa9	/* TEST EAX,imm32 — key disabled */

/*
 * Serialise concurrent calls to unvmed_static_key_enable/disable.
 * Log-level changes are rare admin operations, so a global mutex is fine.
 */
static pthread_mutex_t g_patch_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef __linux__
static int sys_membarrier(int cmd)
{
	return (int)syscall(__NR_membarrier, cmd, 0, 0);
}

/*
 * sync_cores — force every other thread of this process through a core
 * serializing instruction, so none of them keeps executing a stale copy of
 * a branch site we just rewrote.
 *
 * This is the userspace counterpart of the IPI that the kernel's text_poke()
 * issues: MFENCE alone only drains *this* CPU's store buffer and says nothing
 * about instruction fetch on other cores.
 *
 * Correctness does not depend on this — the single-byte patch is safe on its
 * own (see patch_site).  It bounds *latency*: without it a core could keep
 * running the previous encoding until it happens to serialize for some other
 * reason.  Best effort accordingly: the SYNC_CORE command needs a one-time
 * registration and is missing on older kernels, where we settle for making
 * the store globally visible.
 */
static void sync_cores(void)
{
	static _Atomic int state;   /* 0 = unknown, 1 = usable, -1 = unsupported */
	int st = atomic_load_explicit(&state, memory_order_acquire);

	if (st == 0) {
		st = sys_membarrier(
			MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)
			? -1 : 1;
		atomic_store_explicit(&state, st, memory_order_release);
	}

	if (st == 1 &&
	    !sys_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE))
		return;

	/* Fallback: at least make the store globally visible. */
	__asm__ volatile("mfence" ::: "memory");
}
#else
static void sync_cores(void)
{
	__asm__ volatile("mfence" ::: "memory");
}
#endif

/*
 * patch_site — flip the opcode byte of a 5-byte branch site.
 *
 * Exactly one byte is written.  The four displacement bytes that follow are
 * assembler-generated and never change, so a core fetching the site
 * concurrently always decodes a complete, valid, 5-byte instruction — either
 * the old one or the new one.  That is what makes this safe without stopping
 * the world: the danger in cross-modifying code is a torn *instruction*, and
 * a single-byte store cannot tear.
 *
 * The write goes through /proc/self/mem so it never has to mprotect the page
 * writable — which would momentarily drop EXEC and fault any thread running
 * there — and so a site spanning a page boundary is handled by the kernel.
 *
 * The site is never read back: that would be a plain load from memory other
 * threads are executing, a data race with no defined behaviour, and the byte
 * we would learn is one we already know.
 */
static void patch_site(int mem_fd, uint8_t *site, uint8_t opcode)
{
	/* Nothing useful to do on failure: the site keeps its previous
	 * encoding, so the branch stays in its old state rather than
	 * becoming invalid. */
	(void)!pwrite(mem_fd, &opcode, 1, (off_t)(uintptr_t)site);
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

		patch_site(mem_fd, (uint8_t *)((char *)&e->code + e->code),
			   enable ? OP_JMP_REL32 : OP_TEST_EAX);
	}

	close(mem_fd);

	/*
	 * All sites for this key are now rewritten; serialize the other cores
	 * once, rather than per-site, so none of them keeps executing a stale
	 * instruction.
	 */
	sync_cores();
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
