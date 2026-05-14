/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */

#ifndef LIBUNVMED_TRACE_H
#define LIBUNVMED_TRACE_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/*
 * Lightweight in-memory trace ring buffer.  Writes are lock-free and
 * use rdtsc for timestamps to avoid any I/O overhead that would perturb
 * timing-sensitive bugs.
 *
 * This header must be included after libunvmed.h (needs struct unvme_cmd).
 */

enum unvmed_trace_event {
	UNVMED_TRACE_CMD_ALLOC,
	UNVMED_TRACE_CMD_FREE,
	UNVMED_TRACE_CMD_GET,
	UNVMED_TRACE_CMD_PUT,
};

#define UNVMED_TRACE_RING_SIZE	4096	/* must be power of 2 */

struct unvmed_trace_entry {
	uint64_t tsc;		/* rdtsc or CLOCK_MONOTONIC_RAW ns */
	uint8_t  event;		/* enum unvmed_trace_event */
	uint8_t  state;		/* enum unvme_cmd_state at trace point */
	uint16_t sqid;
	uint16_t cid;
	int16_t  refcnt;	/* refcnt value passed by caller */
	void    *caller;	/* __builtin_return_address(0) at call site */
};

struct unvmed_trace_ring {
	atomic_uint_least64_t head;	/* ever-incrementing, index = head % SIZE */
	struct unvmed_trace_entry entries[UNVMED_TRACE_RING_SIZE];
};

extern struct unvmed_trace_ring __unvmed_trace;

static inline uint64_t __unvmed_rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/*
 * __unvmed_trace_record - write one entry into the ring buffer.
 *
 * @caller must be __builtin_return_address(0) evaluated at the macro call
 * site so we record who called cmd_put/get/alloc/free, not this function.
 */
static inline void __unvmed_trace_record(struct unvme_cmd *cmd,
					 enum unvmed_trace_event event,
					 int refcnt, void *caller)
{
	uint64_t idx;
	struct unvmed_trace_entry *e;

	idx = atomic_fetch_add_explicit(&__unvmed_trace.head, 1,
					memory_order_relaxed)
	      & (UNVMED_TRACE_RING_SIZE - 1);
	e = &__unvmed_trace.entries[idx];

	e->tsc    = __unvmed_rdtsc();
	e->event  = (uint8_t)event;
	e->state  = (uint8_t)cmd->state;
	e->sqid   = (uint16_t)cmd->usq->id;
	e->cid    = cmd->cid;
	e->refcnt = (int16_t)refcnt;
	e->caller = caller;
}

#define unvmed_trace(cmd, event, refcnt) \
	__unvmed_trace_record(cmd, event, refcnt, __builtin_return_address(0))

void unvmed_trace_dump(int fd);

#endif
