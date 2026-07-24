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
 *   Format the message into a ring slot with an atomic slot-claim.
 *   No mutex, no syscall, no blocking.  If the ring is full the message
 *   is dropped and n_dropped is incremented.
 *
 * Cold path (logger thread — consumer):
 *   Drains the ring and writes to the log fd.  Sleeps on a condition
 *   variable between drains so it consumes no CPU when idle.
 *
 * Slot ownership protocol (Vyukov bounded MPSC):
 *   - slots[i].seq == write_pos   → slot free, producer may claim it
 *   - slots[i].seq == write_pos+1 → slot ready, consumer may read it
 *   - slots[i].seq == read_pos+RING_SLOTS → slot free for next wrap
 */

#define UNVMED_LOG_RING_SLOTS  (1u << 13)   /* 8 192 — must be power of 2 */
#define UNVMED_LOG_MSG_SIZE    256

struct unvmed_log_slot {
	_Atomic uint64_t seq;
	uint32_t         len;
	char             msg[UNVMED_LOG_MSG_SIZE];
};

struct unvmed_log_ring {
	/* Producer cache line: write_pos claimed with fetch_add */
	_Atomic uint64_t        write_pos;
	char                    _pad[64 - sizeof(_Atomic uint64_t)];

	/* Consumer cache line: read_pos touched only by logger thread */
	uint64_t                read_pos;

	struct unvmed_log_slot  slots[UNVMED_LOG_RING_SLOTS];

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
