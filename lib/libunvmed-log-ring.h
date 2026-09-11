/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_LOG_RING_H
#define LIBUNVMED_LOG_RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>

/*
 * Lock-free MPSC (Multiple Producer, Single Consumer) ring buffer for async
 * log I/O.
 *
 * Hot path (I/O threads — producers):
 *   Format the message on the stack, then claim a ring slot with a CAS loop
 *   and publish it.  No mutex, no syscall, no blocking.  If the ring is full
 *   the message is dropped and n_dropped is incremented.
 *
 * Cold path (logger thread — consumer):
 *   Drains the ring and writes to the log fd.  Sleeps on a condition
 *   variable between drains so it consumes no CPU when idle.
 *
 * Slot ownership protocol (Vyukov bounded MPSC with CAS claim):
 *   - slots[i].seq == write_pos   → slot free, producer may claim it
 *   - slots[i].seq == write_pos+1 → slot ready, consumer may read it
 *   - slots[i].seq == read_pos+RING_SLOTS → slot free for next wrap
 *
 * Unlike the naive fetch_add design, the CAS claim loop only advances
 * write_pos after confirming the slot is actually free.  This means a
 * "drop" never consumes a sequence number, so the consumer is never left
 * waiting for a seq that will never arrive.
 */

#define UNVMED_LOG_RING_SLOTS  (1u << 13)   /* 8 192 — must be power of 2 */
#define UNVMED_LOG_MSG_SIZE    256
/* Upper bound on one rendered log line (a formatted record is longer than
 * the record itself). */
#define UNVMED_LOG_LINE_MAX    512

/*
 * Slot payload kinds.
 *
 * TEXT is a line the caller already rendered; it goes to the text log.
 * Anything else is a binary record and is written verbatim to the trace
 * file, to be rendered only when somebody reads it back.  Nothing is
 * formatted on either the I/O thread or the logger thread — the same split
 * the kernel makes between writing a trace event and reporting it.
 */
#define UNVMED_LOG_REC_TEXT    0

struct unvmed_log_slot {
	_Atomic uint64_t seq;
	uint32_t         len;
	uint8_t          type;
	char             msg[UNVMED_LOG_MSG_SIZE];
};

/*
 * Cache-line layout:
 *   [0..63]  write_pos (producers only)
 *   [64..127] read_pos + pad (consumer only)
 *   [128..]  slots[]
 *
 * Separating write_pos and read_pos onto different cache lines prevents
 * false sharing between producers and the consumer.
 */
struct unvmed_log_ring {
	_Alignas(64) _Atomic uint64_t   write_pos;
	char                            _wp_pad[64 - sizeof(_Atomic uint64_t)];

	_Alignas(64) uint64_t           read_pos;
	char                            _rp_pad[64 - sizeof(uint64_t)];

	struct unvmed_log_slot          slots[UNVMED_LOG_RING_SLOTS];

	/*
	 * In-flight producers: incremented on entry to push() and decremented
	 * once the slot has been published.  unvmed_log_ring_stop() clears
	 * @running and then waits for this to reach zero, so a producer that
	 * passed the @running check can never publish a message after the
	 * logger thread's final drain has already run.
	 *
	 * Producers only touch this on the (already cold) logging path, and it
	 * lives on its own cache line so the RMW does not bounce the line that
	 * carries @write_pos.
	 */
	_Alignas(64) _Atomic uint32_t   n_producers;
	char                            _np_pad[64 - sizeof(_Atomic uint32_t)];

	/* Logger thread */
	pthread_t               thread;
	pthread_mutex_t         lock;
	pthread_cond_t          cond;
	_Atomic bool            running;
	int                     fd;		/* text log */
	int                     fd_trace;	/* binary records, may be -1 */

	/*
	 * Lossless: when the ring is full, make the producer wait for space
	 * instead of dropping the record.  Costs I/O latency exactly when the
	 * log cannot keep up, which is the trade a run being analysed from its
	 * logs wants and a throughput measurement does not.
	 */
	_Atomic bool            lossless;

	_Atomic uint64_t        n_dropped;
	_Atomic uint64_t        n_stalled;	/* pushes that had to wait */
};

/* @fd_trace may be -1 if only UNVMED_LOG_REC_TEXT is ever pushed. */
int  unvmed_log_ring_init(struct unvmed_log_ring *r, int fd, int fd_trace);
void unvmed_log_ring_push(struct unvmed_log_ring *r, uint8_t type,
			  const void *rec, uint32_t len);
void unvmed_log_ring_stop(struct unvmed_log_ring *r);
void unvmed_log_ring_set_lossless(struct unvmed_log_ring *r, bool lossless);

extern struct unvmed_log_ring __log_ring;

#endif /* LIBUNVMED_LOG_RING_H */
