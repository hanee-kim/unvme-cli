/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */

#ifndef LIBUNVMED_TRACE_H
#define LIBUNVMED_TRACE_H

#include <stdint.h>

/*
 * Zero-overhead tracer for unvmed hot paths.
 *
 * Inspired by the Linux kernel's tracepoint + ftrace infrastructure:
 *
 *   Kernel                        │  This implementation
 *   ──────────────────────────────┼──────────────────────────────────────────
 *   static_key_false()            │  __builtin_expect(relaxed-load, 0)
 *   text_poke() nop→jmp           │  branch-predictor learns "not taken"
 *   .text.unlikely placement      │  __attribute__((noinline, cold))
 *   per-CPU ring_buffer           │  global MPSC lock-free ring buffer
 *   ring_buffer_lock_reserve()    │  atomic fetch_add on tail (slot claim)
 *   ring_buffer_unlock_commit()   │  store_release of VALID bit (publish)
 *   TP_fast_assign (binary store) │  struct field writes, no snprintf
 *   TP_printk (format at read)    │  unvmed_trace_drain() formats to fd
 *
 * Hot-path cost when tracing is DISABLED:
 *   1 relaxed load + 1 predicted-not-taken branch ≈ 0 pipeline cycles.
 *
 * Hot-path cost when tracing is ENABLED:
 *   rdtsc + atomic fetch_add + 5× mov + store_release ≈ 15–25 ns.
 *   No snprintf, no syscall, no lock acquisition.
 */

/* ── Event types ─────────────────────────────────────────────────────────── */

enum unvme_trace_evt {
	UNVME_TRACE_CMD_POST = 1, /* SQE posted to NVMe SQ             */
	UNVME_TRACE_CMD_CMPL = 2, /* CQE reaped from NVMe CQ           */
	UNVME_TRACE_VCQ_PUSH = 3, /* CQE pushed to virtual CQ (reaper) */
	UNVME_TRACE_VCQ_POP  = 4, /* CQE popped from virtual CQ (app)  */
};

/* ── Record layout ───────────────────────────────────────────────────────── */

/*
 * VALID bit in evt — the MPSC commit flag.
 *
 * Write order (producer):
 *   1. claim slot via atomic fetch_add on ring->tail  (relaxed)
 *   2. write tsc, sqid, cid, arg0, arg1              (any order)
 *   3. store_release evt | UNVME_TRACE_VALID          ← publish
 *
 * Read order (drain/consumer):
 *   1. load_acquire evt                               ← observe publish
 *   2. if VALID bit set: read remaining fields        (guaranteed visible)
 *   3. store_release evt = 0                          ← clear for reuse
 *
 * This is the same acquire/release pairing the kernel uses between
 * ring_buffer_unlock_commit() and the trace reader.
 */
#define UNVME_TRACE_VALID  (1u << 31)

/*
 * 32 bytes per record:
 *   - power-of-2 → slot index via (idx & mask), no division
 *   - 2 records per 64-byte cache line → cache-friendly sequential drain
 *
 * arg0/arg1 encoding (event-specific):
 *
 *   CMD_POST (read/write):
 *     arg0 = SLBA  (cdw11:cdw10, 64-bit)
 *     arg1 = opcode[63:56] | nsid[55:24] | nlb[15:0]
 *
 *   CMD_POST (admin / other):
 *     arg0 = cdw10 (first command dword, 32-bit in low half)
 *     arg1 = opcode[63:56]
 *
 *   CMD_CMPL / VCQ_PUSH / VCQ_POP:
 *     arg0 = dw1[63:32] | dw0[31:0]
 *     arg1 = sfp[31:16] | sqhd[15:0]
 */
struct unvme_trace_record {
	uint64_t         tsc;  /* rdtsc() — no syscall, ~3 ns on x86     */
	_Atomic uint32_t evt;  /* enum unvme_trace_evt | VALID when ready */
	uint16_t         sqid;
	uint16_t         cid;
	uint64_t         arg0;
	uint64_t         arg1;
};

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

/*
 * 16384 slots × 32 bytes = 512 KB.
 * Sized to hold several seconds of traffic at tens-of-thousands IOPS
 * without wrapping before a drain call.  Overwrite mode (like ftrace's
 * default): when the ring is full, oldest records are silently overwritten.
 */
#define UNVME_TRACE_RING_ORDER  14u
#define UNVME_TRACE_RING_SIZE   (1u << UNVME_TRACE_RING_ORDER)
#define UNVME_TRACE_RING_MASK   (UNVME_TRACE_RING_SIZE - 1u)

/*
 * tail and head are on separate cache lines to prevent false sharing
 * between the producers (which bump tail) and the drain thread (which
 * reads tail and bumps head).  Same layout strategy as Linux's
 * struct ring_buffer_per_cpu.
 */
struct unvme_trace_ring {
	struct unvme_trace_record *buf;

	/* producers' side — written frequently, cache-line aligned */
	_Atomic uint64_t tail;
	char             _pad_tail[56];  /* pad tail to its own cache line */

	/* consumer's side — only the drain caller touches head */
	uint64_t head;
	char     _pad_head[56];
};

/* ── Global state ────────────────────────────────────────────────────────── */

/*
 * __unvme_trace_active — the "static key" equivalent.
 *
 * Written only on enable/disable (rare path).
 * Read on every trace site via relaxed load.
 *
 * Why relaxed?  The worst case is a stale read for a few cycles after
 * enable/disable — completely acceptable for a tracing flag.  Using
 * acquire here would add a memory barrier to every hot-path iteration
 * for no benefit.
 *
 * Linux's static_key achieves true zero cost via text_poke(), which we
 * cannot do in userspace without mprotect() tricks.  The relaxed load +
 * predicted-not-taken branch is the best practical approximation.
 */
extern _Atomic int             __unvme_trace_active;
extern struct unvme_trace_ring __unvme_trace_ring;

/* ── Internal write function ─────────────────────────────────────────────── */

/*
 * __unvme_trace_write — the slow/cold path, called only when tracing is on.
 *
 * __attribute__((noinline)):
 *   Prevents the compiler from inlining this into the hot path, keeping
 *   the hot-path instruction stream small (better i-cache utilization).
 *   Mirrors the kernel's use of noinline on trace handlers.
 *
 * __attribute__((cold)):
 *   Tells the compiler this function is called rarely.  Two effects:
 *     1. Placed in a "cold" region of the binary (away from hot code),
 *        preventing cache-line pollution of the hot path.
 *     2. Optimized for code size rather than speed within the function.
 *   Equivalent to the kernel placing trace handlers in .text.unlikely.
 */
__attribute__((noinline, cold))
void __unvme_trace_write(uint32_t evt, uint16_t sqid, uint16_t cid,
			 uint64_t arg0, uint64_t arg1);

/* ── Hot-path macro ──────────────────────────────────────────────────────── */

/*
 * unvme_trace() — the single entry point for all hot-path trace sites.
 *
 * Assembly generated when tracing is DISABLED (common case):
 *
 *   mov  eax, [__unvme_trace_active]   ; relaxed load — L1 cache hit
 *   test eax, eax
 *   jne  .Lcold                        ; forward branch → CPU predicts NOT TAKEN
 *   ; ... hot path continues inline ...
 *
 * After a few iterations, the branch predictor table learns this branch
 * is "always not taken" and the branch costs effectively 0 cycles —
 * the load feeds directly into the branch unit with no pipeline stall.
 *
 * This matches what the kernel achieves with static_key_false(): a single
 * nop (0 cost) when disabled.  Our approach costs ~1 cycle instead of 0,
 * but requires no kernel privileges and works in any userspace process.
 *
 * When tracing IS enabled the branch is taken and we jump to
 * __unvme_trace_write() which lives in a cold region of the binary.
 *
 * Parameters:
 *   evt         enum unvme_trace_evt
 *   sqid, cid   queue/command identifiers (already available at call sites)
 *   arg0, arg1  event-specific payload (see record layout above)
 */
#define unvme_trace(evt, sqid, cid, arg0, arg1)					\
	do {									\
		if (__builtin_expect(						\
			__atomic_load_n(&__unvme_trace_active,			\
					__ATOMIC_RELAXED), 0))			\
			__unvme_trace_write((uint32_t)(evt),			\
					    (uint16_t)(sqid),			\
					    (uint16_t)(cid),			\
					    (uint64_t)(arg0),			\
					    (uint64_t)(arg1));			\
	} while (0)

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * unvmed_trace_init   — allocate ring buffer; call once at library init.
 * unvmed_trace_fini   — free ring buffer; call at library teardown.
 * unvmed_trace_enable — activate tracing (sets __unvme_trace_active = 1).
 * unvmed_trace_disable— deactivate tracing.
 * unvmed_trace_drain  — format all committed records to @fd, return count.
 *                       Single-consumer: only one caller at a time.
 */
int  unvmed_trace_init(void);
void unvmed_trace_fini(void);
void unvmed_trace_enable(void);
void unvmed_trace_disable(void);
int  unvmed_trace_drain(int fd);

#endif /* LIBUNVMED_TRACE_H */
