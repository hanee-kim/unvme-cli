// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "libunvmed-log-ring.h"

struct unvmed_log_ring __log_ring;

/*
 * write_all — write exactly @len bytes, restarting on EINTR.
 * Silently truncates on other errors (ENOSPC, EBADF, etc.).
 */
static void write_all(int fd, const char *buf, uint32_t len)
{
	uint32_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (uint32_t)n;
	}
}

/*
 * ring_pop — consumer-only, called only from the logger thread.
 *
 * Returns true and copies the message when a ready slot is found.
 * Advances read_pos and marks the slot free for future producers.
 */
static bool ring_pop(struct unvmed_log_ring *r, char *out, uint32_t *out_len)
{
	uint64_t pos = r->read_pos;
	uint64_t idx = pos & (UNVMED_LOG_RING_SLOTS - 1);
	struct unvmed_log_slot *slot = &r->slots[idx];

	/* Slot is ready when seq == pos + 1 */
	if (atomic_load_explicit(&slot->seq, memory_order_acquire) != pos + 1)
		return false;

	uint32_t len = slot->len;
	if (len > UNVMED_LOG_MSG_SIZE)
		len = UNVMED_LOG_MSG_SIZE;
	*out_len = len;
	memcpy(out, slot->msg, len);

	/* Release slot back to producers: seq = pos + RING_SLOTS */
	atomic_store_explicit(&slot->seq,
			      pos + UNVMED_LOG_RING_SLOTS,
			      memory_order_release);
	r->read_pos++;
	return true;
}

static void *logger_thread(void *arg)
{
	struct unvmed_log_ring *r = arg;
	char     buf[UNVMED_LOG_MSG_SIZE];
	uint32_t len;
	struct timespec ts;

	while (atomic_load_explicit(&r->running, memory_order_acquire)) {
		/* Drain every ready slot without sleeping */
		while (ring_pop(r, buf, &len))
			write_all(r->fd, buf, len);

		/*
		 * Sleep until a producer signals or 1 ms elapses.
		 * The 1 ms backstop catches signals that arrived while we
		 * were not yet inside pthread_cond_timedwait.
		 */
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_mutex_lock(&r->lock);
		pthread_cond_timedwait(&r->cond, &r->lock, &ts);
		pthread_mutex_unlock(&r->lock);
	}

	/* Final drain — flush whatever producers published before stop */
	while (ring_pop(r, buf, &len))
		write_all(r->fd, buf, len);

	return NULL;
}

int unvmed_log_ring_init(struct unvmed_log_ring *r, int fd)
{
	int rc;

	/* Initialise every slot's sequence number to its own index so that
	 * slot[i] is immediately claimable by a producer whose write_pos
	 * modulo RING_SLOTS equals i. */
	for (unsigned i = 0; i < UNVMED_LOG_RING_SLOTS; i++)
		atomic_init(&r->slots[i].seq, (uint64_t)i);

	atomic_init(&r->write_pos, 0);
	atomic_init(&r->n_dropped, 0);
	atomic_init(&r->running,   true);
	r->read_pos = 0;
	r->fd       = fd;

	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->cond,  NULL);

	rc = pthread_create(&r->thread, NULL, logger_thread, r);
	if (rc != 0) {
		atomic_store_explicit(&r->running, false, memory_order_relaxed);
		pthread_cond_destroy(&r->cond);
		pthread_mutex_destroy(&r->lock);
	}
	return rc;
}

/*
 * unvmed_log_ring_push — called from the hot path (I/O threads).
 *
 * Uses a CAS loop to claim a slot only when it is actually free.  Unlike a
 * naked fetch_add, a failed claim does NOT consume a sequence number, so the
 * consumer never waits forever for a seq that was dropped and never published.
 *
 * Never blocks: if the ring is full the message is dropped.
 */
void unvmed_log_ring_push(struct unvmed_log_ring *r,
			  const char *msg, uint32_t len)
{
	struct unvmed_log_slot *slot;
	uint64_t pos, seq;
	int64_t  dif;

	/* Bail out early if the ring has been stopped. */
	if (!atomic_load_explicit(&r->running, memory_order_acquire))
		return;

	pos = atomic_load_explicit(&r->write_pos, memory_order_relaxed);

	for (;;) {
		slot = &r->slots[pos & (UNVMED_LOG_RING_SLOTS - 1)];
		seq  = atomic_load_explicit(&slot->seq, memory_order_acquire);
		dif  = (int64_t)(seq - pos);   /* signed: wrap-safe */

		if (dif == 0) {
			/* Slot is free; try to claim it. */
			if (atomic_compare_exchange_weak_explicit(
					&r->write_pos, &pos, pos + 1,
					memory_order_relaxed,
					memory_order_relaxed))
				break;          /* claimed — pos still holds our value */
			/* CAS lost: pos was updated to the current write_pos;
			 * retry with the new value. */
		} else if (dif < 0) {
			/* Ring full — drop without advancing write_pos. */
			atomic_fetch_add_explicit(&r->n_dropped, 1,
						  memory_order_relaxed);
			return;
		} else {
			/* Another producer is ahead; re-read write_pos. */
			pos = atomic_load_explicit(&r->write_pos,
						   memory_order_relaxed);
		}
	}

	uint32_t n = (len < UNVMED_LOG_MSG_SIZE) ? len : UNVMED_LOG_MSG_SIZE - 1;
	memcpy(slot->msg, msg, n);
	slot->len = n;

	/* Publish to consumer */
	atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);

	/*
	 * Signal the logger thread.  trylock avoids ever blocking the hot
	 * path: if the consumer holds the lock it is already awake and will
	 * drain this entry on its next iteration.
	 */
	if (!pthread_mutex_trylock(&r->lock)) {
		pthread_cond_signal(&r->cond);
		pthread_mutex_unlock(&r->lock);
	}
}

void unvmed_log_ring_stop(struct unvmed_log_ring *r)
{
	bool was_running;

	/* Idempotent: do nothing if already stopped. */
	was_running = atomic_exchange_explicit(&r->running, false,
					       memory_order_acq_rel);
	if (!was_running)
		return;

	/*
	 * Wake the logger thread.  Use trylock so that if this function is
	 * called from a signal handler that interrupted a thread holding
	 * r->lock, we do not deadlock.  If trylock fails, the thread is
	 * already awake; it will see running==false on its next check and
	 * exit within at most 1 ms (the timedwait timeout).
	 */
	if (!pthread_mutex_trylock(&r->lock)) {
		pthread_cond_signal(&r->cond);
		pthread_mutex_unlock(&r->lock);
	}

	pthread_join(r->thread, NULL);

	/* Report any dropped messages. */
	uint64_t dropped = atomic_load_explicit(&r->n_dropped,
						memory_order_relaxed);
	if (dropped > 0 && r->fd >= 0) {
		char dmsg[80];
		int  dlen = snprintf(dmsg, sizeof(dmsg),
				     "log: %llu message(s) dropped (ring full)\n",
				     (unsigned long long)dropped);
		if (dlen > 0 && (uint32_t)dlen < sizeof(dmsg))
			write_all(r->fd, dmsg, (uint32_t)dlen);
	}

	/*
	 * Do NOT call pthread_mutex_destroy / pthread_cond_destroy here.
	 * Other threads (I/O threads whose shutdown races with ours) may
	 * still call push(), which does pthread_mutex_trylock.  The
	 * running==false guard in push() returns early before any lock
	 * access in the common case, but there is a tiny window between
	 * the load of running and the trylock.  Leaving the primitives
	 * alive is safe — the process exits shortly after stop() is called.
	 */
}
