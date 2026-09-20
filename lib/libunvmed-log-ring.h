/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_LOG_RING_H
#define LIBUNVMED_LOG_RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>

/*
 * Lock-free MPSC ring buffer handing log payloads from the I/O threads to a
 * dedicated writer thread, so logging never calls write(2) on the I/O path.
 *
 * Slot ownership (Vyukov bounded MPSC, claimed by CAS):
 *   slots[i].seq == write_pos              free, a producer may claim it
 *   slots[i].seq == write_pos + 1          published, the consumer may read
 *   slots[i].seq == read_pos + RING_SLOTS  released for the next wrap
 *
 * The claim is a CAS rather than a fetch_add so that abandoning it consumes
 * no sequence number; a consumer waiting on a number that is never published
 * would stall the ring for good.
 */

#define UNVMED_LOG_RING_SLOTS  (1u << 13)   /* must be a power of 2 */
#define UNVMED_LOG_MSG_SIZE    256

/*
 * TEXT is a line the caller already rendered and goes to the text log; any
 * other kind is an opaque record written verbatim to the trace file, to be
 * rendered only when read back.
 */
#define UNVMED_LOG_REC_TEXT    0

struct unvmed_log_slot {
	_Atomic uint64_t seq;
	uint32_t         len;
	uint8_t          type;
	char             msg[UNVMED_LOG_MSG_SIZE];
};

/* write_pos, read_pos and n_producers each get their own cache line: the
 * producers and the consumer would otherwise share one. */
struct unvmed_log_ring {
	_Alignas(64) _Atomic uint64_t   write_pos;
	char                            _wp_pad[64 - sizeof(_Atomic uint64_t)];

	_Alignas(64) uint64_t           read_pos;
	char                            _rp_pad[64 - sizeof(uint64_t)];

	struct unvmed_log_slot          slots[UNVMED_LOG_RING_SLOTS];

	/*
	 * Producers in flight: raised on entry to push(), dropped once the
	 * slot is published.  The logger thread waits for this to reach zero
	 * before its final drain, so a producer that passed the @running check
	 * cannot publish after the ring has already been drained.
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

	/* Wait for ring space instead of dropping.  Trades I/O latency for a
	 * complete log, and only bites when the log cannot keep up. */
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
