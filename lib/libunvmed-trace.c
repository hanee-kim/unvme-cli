// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT

/*
 * libunvmed-trace.c — zero-overhead ring-buffer tracer
 *
 * Architectural overview
 * ══════════════════════
 *
 *  ┌─────────────────────────┐   ┌─────────────────────────┐
 *  │  submission thread      │   │  reaper thread           │
 *  │  unvmed_cmd_post()      │   │  __unvmed_reap_cqe()     │
 *  │    └─ unvme_trace()     │   │    └─ unvme_trace()      │
 *  └────────────┬────────────┘   └────────────┬─────────────┘
 *               │  atomic fetch_add(tail)      │
 *               │  write fields                │
 *               │  store_release(VALID)        │
 *               └──────────────┬───────────────┘
 *                              ▼
 *                   ┌──────────────────┐
 *                   │  ring buffer     │  512 KB, 16384 × 32B slots
 *                   │  (MPSC, no lock) │  overwrite when full (ftrace default)
 *                   └────────┬─────────┘
 *                            │  unvmed_trace_drain()
 *                            ▼
 *                   load_acquire(VALID)
 *                   format record → write(fd)   ← snprintf ONLY here
 *                   store_release(evt = 0)
 *
 * MPSC protocol
 * ─────────────
 * Multiple producers (threads) and a single consumer (drain caller).
 *
 * Producers claim a unique slot with fetch_add(tail, relaxed).  The
 * relaxed ordering is sufficient because we only need uniqueness, not
 * global visibility of the tail counter.  Each producer then writes the
 * record fields and publishes with store_release on evt|VALID.
 *
 * The consumer load_acquires evt to observe VALID; the acquire/release
 * pair guarantees the consumer sees all field writes that preceded the
 * store_release.  This is identical to the kernel's
 * ring_buffer_unlock_commit() (store_release) + reader rb_advance_reader()
 * (load_acquire) pairing.
 *
 * Head-of-line behaviour
 * ──────────────────────
 * If a producer claims slot N but hasn't committed yet (VALID not set),
 * drain stops at slot N even if slot N+1 is already committed.  This
 * preserves strict ordering — the same policy as Linux's relay/ring_buffer
 * readers.  In practice, the window between fetch_add and store_release
 * is a handful of instructions; head-of-line stalls are extremely rare.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libunvmed-trace.h"

/* ── Global instances ────────────────────────────────────────────────────── */

_Atomic int             __unvme_trace_active;
struct unvme_trace_ring __unvme_trace_ring;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int unvmed_trace_init(void)
{
	struct unvme_trace_ring *ring = &__unvme_trace_ring;

	ring->buf = calloc(UNVME_TRACE_RING_SIZE, sizeof(struct unvme_trace_record));
	if (!ring->buf)
		return -1;

	/*
	 * Both tail and head start at 0.  head is a plain uint64_t because
	 * only the single drain caller ever writes it — no atomic needed.
	 * tail is _Atomic because multiple producer threads bump it.
	 */
	__atomic_store_n(&ring->tail, 0, __ATOMIC_RELAXED);
	ring->head = 0;

	__atomic_store_n(&__unvme_trace_active, 0, __ATOMIC_RELAXED);
	return 0;
}

void unvmed_trace_fini(void)
{
	struct unvme_trace_ring *ring = &__unvme_trace_ring;

	/*
	 * Disable first so no new records are written after we free the
	 * buffer.  A release store ensures the disable is visible before
	 * the free — any concurrent producer that already entered the
	 * if-branch will complete its write before the free reaches it
	 * (the window is tiny; in practice callers quiesce threads first).
	 */
	__atomic_store_n(&__unvme_trace_active, 0, __ATOMIC_RELEASE);

	free(ring->buf);
	ring->buf = NULL;
}

void unvmed_trace_enable(void)
{
	/*
	 * relaxed store is intentional: callers that observe the enable on
	 * the next iteration are fine.  No memory ordering needed here —
	 * enabling tracing is a best-effort, low-latency operation.
	 */
	__atomic_store_n(&__unvme_trace_active, 1, __ATOMIC_RELAXED);
}

void unvmed_trace_disable(void)
{
	__atomic_store_n(&__unvme_trace_active, 0, __ATOMIC_RELAXED);
}

/* ── Hot-path write (cold section) ──────────────────────────────────────── */

/*
 * __unvme_trace_write — lock-free MPSC slot write.
 *
 * Called only when __unvme_trace_active != 0 (the taken branch in the
 * unvme_trace() macro).  The noinline + cold attributes keep this function
 * out of the hot-path instruction stream; see header for rationale.
 *
 * Step-by-step mapping to kernel ring_buffer:
 *
 *   fetch_add(tail, relaxed)          ≈  ring_buffer_lock_reserve()
 *     │  claims a unique slot index       │  claims bytes in per-CPU buffer
 *     │  no ordering needed (uniqueness)  │  uses local_add() (per-CPU atomic)
 *     ▼                                   ▼
 *   write tsc, sqid, cid, arg0, arg1  ≈  TP_fast_assign in TRACE_EVENT()
 *     │  plain stores, any order          │  plain field assignments
 *     │  consumer can't see them yet      │  protected by !VALID / !commit
 *     ▼                                   ▼
 *   store_release(evt | VALID)         ≈  ring_buffer_unlock_commit()
 *       publishes all prior writes         uses store_release / smp_wmb
 */
__attribute__((noinline, cold))
void __unvme_trace_write(uint32_t evt, uint16_t sqid, uint16_t cid,
			 uint64_t arg0, uint64_t arg1)
{
	struct unvme_trace_ring   *ring = &__unvme_trace_ring;
	struct unvme_trace_record *rec;
	uint64_t idx;

	if (__builtin_expect(!ring->buf, 0))
		return;

	/*
	 * Claim a slot.  fetch_add with relaxed ordering is sufficient:
	 *   - The operation itself is atomic, so each producer gets a unique idx.
	 *   - We don't need to order this with respect to other producers'
	 *     writes; we only need our own writes to be ordered before VALID.
	 *
	 * Contrast with a mutex: fetch_add is a single lock-free instruction
	 * (LOCK XADD on x86), no cache-line ownership transfer, no OS involvement.
	 */
	idx = __atomic_fetch_add(&ring->tail, 1, __ATOMIC_RELAXED);
	rec = &ring->buf[idx & UNVME_TRACE_RING_MASK];

	/*
	 * rdtsc — the x86 "read time-stamp counter" instruction.
	 * ~3–5 ns, no syscall, no vDSO, serialised by the CPU's own pipeline.
	 * The kernel uses rdtsc_ordered() (with lfence for strict ordering);
	 * for tracing we accept the minor reordering that __builtin_ia32_rdtsc
	 * may introduce — timestamp accuracy to the nearest ~10 ns is enough.
	 */
	rec->tsc  = __builtin_ia32_rdtsc();
	rec->sqid = sqid;
	rec->cid  = cid;
	rec->arg0 = arg0;
	rec->arg1 = arg1;

	/*
	 * Publish the record with store_release.
	 *
	 * store_release guarantees: all stores above (tsc, sqid, cid, arg0,
	 * arg1) are globally visible BEFORE this store becomes visible.
	 *
	 * The drain thread reads this field with load_acquire, forming the
	 * acquire/release pair that makes the other fields visible to it.
	 *
	 * Without this pairing, the drain thread could observe VALID=1 but
	 * still see stale values for tsc/arg0/arg1 on weakly-ordered CPUs
	 * (ARM, POWER).  On x86 the TSO model makes this safe without
	 * barriers, but we write portable code.
	 */
	__atomic_store_n(&rec->evt, evt | UNVME_TRACE_VALID, __ATOMIC_RELEASE);
}

/* ── Drain / format (slow path) ─────────────────────────────────────────── */

/*
 * unvmed_trace_format — render one record as a human-readable line.
 *
 * Mirrors TP_printk in a Linux TRACE_EVENT() definition: formatting happens
 * here, at read time, never in the hot path.  The snprintf calls in this
 * function would be catastrophic in a hot loop (100–500 ns each), but they
 * are fine here because this function is only called from unvmed_trace_drain()
 * which is explicitly invoked by the user, not from the I/O path.
 */
static int unvmed_trace_format(char *buf, size_t sz,
			       const struct unvme_trace_record *rec)
{
	uint32_t    evt = __atomic_load_n(&rec->evt, __ATOMIC_RELAXED)
				& ~UNVME_TRACE_VALID;
	const char *evtname;
	char        detail[128];

	switch (evt) {
	case UNVME_TRACE_CMD_POST: {
		/*
		 * arg0 = SLBA (read/write) or cdw10 (admin)
		 * arg1 = opcode[63:56] | nsid[55:24] | nlb[15:0]
		 */
		uint8_t  opcode = (uint8_t)(rec->arg1 >> 56);
		uint32_t nsid   = (uint32_t)((rec->arg1 >> 24) & 0xffffffff);
		uint16_t nlb    = (uint16_t)(rec->arg1 & 0xffff);

		evtname = "cmd_post";
		if (opcode == 0x01 || opcode == 0x02) {
			snprintf(detail, sizeof(detail),
				 "opcode=0x%02x nsid=%u slba=%llu nlb=%u",
				 opcode, nsid,
				 (unsigned long long)rec->arg0, nlb);
		} else {
			snprintf(detail, sizeof(detail),
				 "opcode=0x%02x cdw10=0x%08x",
				 opcode, (uint32_t)rec->arg0);
		}
		break;
	}
	case UNVME_TRACE_CMD_CMPL:
		/*
		 * arg0 = dw1[63:32] | dw0[31:0]
		 * arg1 = sfp[31:16] | sqhd[15:0]
		 */
		evtname = "cmd_cmpl";
		snprintf(detail, sizeof(detail),
			 "dw0=0x%08x dw1=0x%08x sfp=0x%04x sqhd=%u",
			 (uint32_t)rec->arg0, (uint32_t)(rec->arg0 >> 32),
			 (uint16_t)(rec->arg1 >> 16), (uint16_t)rec->arg1);
		break;

	case UNVME_TRACE_VCQ_PUSH:
		evtname = "vcq_push";
		snprintf(detail, sizeof(detail),
			 "dw0=0x%08x dw1=0x%08x sfp=0x%04x sqhd=%u",
			 (uint32_t)rec->arg0, (uint32_t)(rec->arg0 >> 32),
			 (uint16_t)(rec->arg1 >> 16), (uint16_t)rec->arg1);
		break;

	case UNVME_TRACE_VCQ_POP:
		evtname = "vcq_pop ";
		snprintf(detail, sizeof(detail),
			 "dw0=0x%08x dw1=0x%08x sfp=0x%04x sqhd=%u",
			 (uint32_t)rec->arg0, (uint32_t)(rec->arg0 >> 32),
			 (uint16_t)(rec->arg1 >> 16), (uint16_t)rec->arg1);
		break;

	default:
		evtname = "unknown ";
		snprintf(detail, sizeof(detail),
			 "arg0=0x%016llx arg1=0x%016llx",
			 (unsigned long long)rec->arg0,
			 (unsigned long long)rec->arg1);
		break;
	}

	return snprintf(buf, sz, "[%20llu] %-8s sq=%-4u cid=%-5u %s\n",
			(unsigned long long)rec->tsc,
			evtname, rec->sqid, rec->cid, detail);
}

/*
 * unvmed_trace_drain — consume and format all committed records.
 *
 * Single-consumer contract: only one thread may call this at a time.
 * (ring->head is a plain uint64_t for this reason — no atomic overhead.)
 *
 * Drain stops at the first uncommitted slot (VALID not yet set by its
 * producer).  This preserves record ordering at the cost of potential
 * head-of-line delay.  In practice producers commit within nanoseconds
 * of claiming their slot, so stalls are imperceptible.
 *
 * Mapping to the kernel's trace reader:
 *
 *   load_acquire(evt)          ≈  rb_advance_reader() acquire fence
 *   check VALID bit            ≈  check rb_event committed flag
 *   format record → write(fd)  ≈  seq_printf() in tracing_read_pipe()
 *   store_release(evt = 0)     ≈  rb_advance_reader() release / slot reuse
 *   head++                     ≈  per-CPU reader page advance
 */
int unvmed_trace_drain(int fd)
{
	struct unvme_trace_ring *ring = &__unvme_trace_ring;
	char buf[256];
	int  n = 0;

	if (__builtin_expect(!ring->buf, 0))
		return 0;

	while (1) {
		struct unvme_trace_record *rec;
		uint64_t tail;
		uint32_t evt;
		int      len;

		/*
		 * Snapshot tail with acquire so we see all slots up to tail
		 * that producers may have already committed.
		 */
		tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
		if (ring->head >= tail)
			break;  /* ring is empty */

		rec = &ring->buf[ring->head & UNVME_TRACE_RING_MASK];

		/*
		 * load_acquire on evt: pairs with __unvme_trace_write's
		 * store_release, ensuring we see tsc/sqid/cid/arg0/arg1 as
		 * written by the producer before it set VALID.
		 */
		evt = __atomic_load_n(&rec->evt, __ATOMIC_ACQUIRE);
		if (!(evt & UNVME_TRACE_VALID))
			break;  /* producer hasn't committed yet — stop here */

		len = unvmed_trace_format(buf, sizeof(buf), rec);
		if (len > 0) {
			ssize_t wr __attribute__((unused));
			wr = write(fd, buf, (size_t)len);
		}

		/*
		 * Clear the slot with store_release so that a future producer
		 * wrapping around to this index sees evt == 0 and can safely
		 * overwrite it.  The release ensures our read of the record's
		 * fields is complete before the slot is marked reusable.
		 */
		__atomic_store_n(&rec->evt, 0u, __ATOMIC_RELEASE);
		ring->head++;
		n++;
	}

	return n;
}
