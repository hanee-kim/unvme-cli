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
 * Both fields are signed offsets from their own address to their referent,
 * so the referent is &field + field with no runtime relocation.  The branch
 * target is not recorded: the assembler baked it into the site.
 */
struct __unvmed_jump_entry {
	int64_t code;
	int64_t key;
};

/*
 * Section bounds synthesised by the linker, covering this object only.
 *
 * Weak: the linker only creates them if the section exists, so a build with
 * no static-branch site left would otherwise fail to link from here.  They
 * resolve to NULL instead and the scan finds no entries.
 */
extern struct __unvmed_jump_entry __start___unvme_jump_table[]
	__attribute__((weak));
extern struct __unvmed_jump_entry __stop___unvme_jump_table[]
	__attribute__((weak));

#ifdef __x86_64__

/* Interchangeable: both consume the 4-byte displacement that follows. */
#define OP_JMP_REL32	0xe9	/* JMP rel32      - key enabled  */
#define OP_TEST_EAX	0xa9	/* TEST EAX,imm32 - key disabled */

/* Level changes are rare, so one mutex for all keys is enough. */
static pthread_mutex_t g_patch_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef __linux__
static int sys_membarrier(int cmd)
{
	return (int)syscall(__NR_membarrier, cmd, 0, 0);
}

/*
 * Push other threads through a core serializing instruction, the userspace
 * counterpart of the IPI text_poke() issues: MFENCE drains this CPU's store
 * buffer but says nothing about instruction fetch elsewhere.
 *
 * Only bounds latency - the single-byte patch is safe without it - so this
 * is best effort and degrades to MFENCE where the kernel lacks SYNC_CORE.
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
 * Flip the opcode byte of a branch site.  A single-byte store cannot tear,
 * which is what makes this safe against concurrent execution.
 *
 * /proc/self/mem rather than mprotect(): mprotect would momentarily drop
 * EXEC and fault any thread running there, and this also lets the kernel
 * handle a site spanning a page boundary.  The site is never read back -
 * that would be a data race with code other threads are executing.
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
