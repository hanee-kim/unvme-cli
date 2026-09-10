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

struct unvmed_log_slot {
	_Atomic uint64_t seq;
	uint32_t         len;
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

	/* Logger thread */
	pthread_t               thread;
	pthread_mutex_t         lock;
	pthread_cond_t          cond;
	_Atomic bool            running;
	int                     fd;

	_Atomic uint64_t        n_dropped;
};

int  unvmed_log_ring_init(struct unvmed_log_ring *r, int fd);
void unvmed_log_ring_push(struct unvmed_log_ring *r,
			  const char *msg, uint32_t len);
void unvmed_log_ring_stop(struct unvmed_log_ring *r);

extern struct unvmed_log_ring __log_ring;

#endif /* LIBUNVMED_LOG_RING_H */
