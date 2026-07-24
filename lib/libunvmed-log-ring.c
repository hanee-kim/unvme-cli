// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "libunvmed-log-ring.h"

struct unvmed_log_ring __log_ring;

/*
 * ring_pop — consumer-only, called only from the logger thread.
 *
 * Returns true and copies the message when a ready slot is found.
 * Advances read_pos and marks the slot free for future producers.
 */
static bool ring_pop(struct unvmed_log_ring *r, char *out, uint32_t *out_len)
{
	uint64_t pos  = r->read_pos;
	uint64_t idx  = pos & (UNVMED_LOG_RING_SLOTS - 1);
	struct unvmed_log_slot *slot = &r->slots[idx];

	/* Slot is ready when seq == pos + 1 */
	if (atomic_load_explicit(&slot->seq, memory_order_acquire) != pos + 1)
		return false;

	*out_len = slot->len;
	memcpy(out, slot->msg, slot->len);

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

	while (atomic_load_explicit(&r->running, memory_order_relaxed)) {
		/* Drain every ready slot without sleeping */
		while (ring_pop(r, buf, &len))
			write(r->fd, buf, len);

		/*
		 * Sleep until a producer signals or 1 ms elapses.
		 * The 1 ms backstop catches signals that arrived while we
		 * were not yet in pthread_cond_timedwait.
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

	/* Final drain — flush whatever producers pushed before stop */
	while (ring_pop(r, buf, &len))
		write(r->fd, buf, len);

	return NULL;
}

int unvmed_log_ring_init(struct unvmed_log_ring *r, int fd)
{
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

	return pthread_create(&r->thread, NULL, logger_thread, r);
}

/*
 * unvmed_log_ring_push — called from the hot path (I/O threads).
 *
 * Claims a slot with an atomic fetch_add, writes the pre-formatted
 * message, then publishes it for the consumer.  Never blocks: if the
 * ring is full the message is silently dropped.
 */
void unvmed_log_ring_push(struct unvmed_log_ring *r,
			  const char *msg, uint32_t len)
{
	uint64_t pos  = atomic_fetch_add_explicit(&r->write_pos, 1,
						  memory_order_relaxed);
	uint64_t idx  = pos & (UNVMED_LOG_RING_SLOTS - 1);
	struct unvmed_log_slot *slot = &r->slots[idx];

	/* Ring full: the slot we claimed is not yet consumed */
	if (atomic_load_explicit(&slot->seq, memory_order_acquire) != pos) {
		atomic_fetch_add_explicit(&r->n_dropped, 1,
					  memory_order_relaxed);
		return;
	}

	uint32_t n = (len < UNVMED_LOG_MSG_SIZE) ? len : UNVMED_LOG_MSG_SIZE - 1;
	memcpy(slot->msg, msg, n);
	slot->len = n;

	/* Publish to consumer */
	atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);

	/*
	 * Signal the logger thread.  trylock avoids ever blocking the hot
	 * path: if the consumer holds the lock it is already awake and will
	 * drain the new entry on its next iteration.
	 */
	if (!pthread_mutex_trylock(&r->lock)) {
		pthread_cond_signal(&r->cond);
		pthread_mutex_unlock(&r->lock);
	}
}

void unvmed_log_ring_stop(struct unvmed_log_ring *r)
{
	atomic_store_explicit(&r->running, false, memory_order_relaxed);

	/* Wake the thread so it exits its timedwait immediately */
	pthread_mutex_lock(&r->lock);
	pthread_cond_signal(&r->cond);
	pthread_mutex_unlock(&r->lock);

	pthread_join(r->thread, NULL);

	pthread_cond_destroy(&r->cond);
	pthread_mutex_destroy(&r->lock);
}
