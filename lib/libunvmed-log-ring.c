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

/*
 * Batched output.
 *
 * One write(2) per message caps the drain rate at roughly one syscall's worth
 * of work per message, which is far slower than producers can fill the ring —
 * so the ring runs full and messages get dropped even though the consumer is
 * doing nothing but writing.  Accumulating into a buffer and issuing one
 * write per batch removes the syscall from the per-message cost.
 */
#define LOG_BATCH_SIZE	(64 * 1024)

struct log_batch {
	size_t n;
	char   b[LOG_BATCH_SIZE];
};

static void batch_flush(int fd, struct log_batch *w)
{
	if (w->n) {
		write_all(fd, w->b, (uint32_t)w->n);
		w->n = 0;
	}
}

/*
 * Drain the ring straight into the batch buffer — ring_pop() copies directly
 * to its final location, so a message is copied once, not twice.
 */
static void drain(struct unvmed_log_ring *r, struct log_batch *w)
{
	uint32_t len;

	for (;;) {
		/* Reserve room for the largest message before popping into it. */
		if (w->n + UNVMED_LOG_MSG_SIZE > sizeof(w->b))
			batch_flush(r->fd, w);

		if (!ring_pop(r, w->b + w->n, &len))
			return;

		w->n += len;
	}
}

static void *logger_thread(void *arg)
{
	struct unvmed_log_ring *r = arg;
	struct log_batch w = { .n = 0 };
	struct timespec ts;

	while (atomic_load_explicit(&r->running, memory_order_acquire)) {
		/* Drain every ready slot without sleeping */
		drain(r, &w);

		/*
		 * Flush before sleeping.  Batching may hold messages back, but
		 * only while there is more to drain — a log that goes quiet
		 * must not leave its last lines sitting in the buffer.
		 */
		batch_flush(r->fd, &w);

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

	/*
	 * Shutdown drain.
	 *
	 * Clearing @running does not mean the ring is quiet: a producer that
	 * had already passed the @running check may still be between claiming
	 * its slot and publishing it.  Draining once and exiting here would
	 * lose every such message — and, worse, lose everything queued behind
	 * it, because a claimed-but-unpublished slot is a hole that stops
	 * ring_pop() dead and strands the entire rest of the ring.
	 *
	 * So keep draining until the in-flight producer count reaches zero.
	 * At that point no claims are outstanding, so no holes remain, and the
	 * final pass below is guaranteed to reach the end of the ring.
	 *
	 * Bounded (~seconds) so a wedged or SIGSTOPped producer cannot hang
	 * shutdown forever.
	 */
	for (int i = 0; i < 100000; i++) {
		drain(r, &w);

		if (!atomic_load_explicit(&r->n_producers, memory_order_seq_cst))
			break;

		usleep(1);
	}

	/* Producers have quiesced — drain what they published on the way out. */
	drain(r, &w);
	batch_flush(r->fd, &w);

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
	atomic_init(&r->n_producers, 0);
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

	/*
	 * Announce ourselves as an in-flight producer *before* testing
	 * @running, and test @running with seq_cst.
	 *
	 * This is the store-load (Dekker) half of the shutdown handshake:
	 * unvmed_log_ring_stop() stores running=false, and the logger thread
	 * then waits for n_producers to fall to zero before its final drain,
	 * both seq_cst.  Sequential consistency is required — plain
	 * acquire/release permits the store and the load to be reordered on
	 * both sides, which would let each miss the other and resurrect the
	 * lost-message race this handshake exists to close.
	 *
	 * The guarantee: if this thread goes on to claim a slot, the logger
	 * thread cannot finish draining until we have published it.
	 */
	atomic_fetch_add_explicit(&r->n_producers, 1, memory_order_seq_cst);

	if (!atomic_load_explicit(&r->running, memory_order_seq_cst)) {
		atomic_fetch_sub_explicit(&r->n_producers, 1,
					  memory_order_release);
		return;
	}

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
			atomic_fetch_sub_explicit(&r->n_producers, 1,
						  memory_order_release);
			return;
		} else {
			/* Another producer is ahead; re-read write_pos. */
			pos = atomic_load_explicit(&r->write_pos,
						   memory_order_relaxed);
		}
	}

	/*
	 * The consumer writes exactly @len bytes and never treats the payload
	 * as a C string, so the full slot is usable — clamping to
	 * UNVMED_LOG_MSG_SIZE - 1 would silently shear the trailing newline
	 * off a message that exactly fills the slot.
	 */
	uint32_t n = (len < UNVMED_LOG_MSG_SIZE) ? len : UNVMED_LOG_MSG_SIZE;
	memcpy(slot->msg, msg, n);
	slot->len = n;

	/* Publish to consumer */
	atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);

	/*
	 * Release our in-flight reference only after publishing.  Until this
	 * point unvmed_log_ring_stop() will keep waiting, guaranteeing the
	 * logger thread's final drain runs after this message is visible.
	 */
	atomic_fetch_sub_explicit(&r->n_producers, 1, memory_order_release);

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

	/*
	 * Idempotent: do nothing if already stopped.
	 *
	 * seq_cst (not acq_rel): this store is the other half of the shutdown
	 * handshake described in push(), pairing with push()'s seq_cst load of
	 * @running.  The matching wait on @n_producers lives in the logger
	 * thread, which is the side that actually drains; pthread_join() below
	 * therefore already waits for it.
	 */
	was_running = atomic_exchange_explicit(&r->running, false,
					       memory_order_seq_cst);
	if (!was_running)
		return;

	/*
	 * Cannot join ourselves.  Reachable only if the logger thread itself
	 * ends up here (e.g. an exit(3) from inside write_all()'s call stack);
	 * @running is already cleared, so the thread will drain and unwind on
	 * its own.  Joining would deadlock (EDEADLK at best).
	 */
	if (pthread_equal(pthread_self(), r->thread))
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
		/* (size_t), not (uint32_t): the comparison must stay in the
		 * same type as sizeof so a truncating snprintf is detected. */
		if (dlen > 0 && (size_t)dlen < sizeof(dmsg))
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
