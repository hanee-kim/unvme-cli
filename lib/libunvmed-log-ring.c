// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "libunvmed-log-ring.h"

struct unvmed_log_ring __log_ring;

/* Write exactly @len bytes, restarting on EINTR.  Other errors (ENOSPC,
 * EBADF) truncate silently: there is nowhere to report them to. */
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

/* Consumer side, logger thread only.  Copies out a published slot and
 * releases it back to the producers. */
static bool ring_pop(struct unvmed_log_ring *r, char *out, uint32_t *out_len,
		     uint8_t *out_type)
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
	*out_type = slot->type;
	memcpy(out, slot->msg, len);

	/* Release slot back to producers: seq = pos + RING_SLOTS */
	atomic_store_explicit(&slot->seq,
			      pos + UNVMED_LOG_RING_SLOTS,
			      memory_order_release);
	r->read_pos++;
	return true;
}

/*
 * One write(2) per message caps the drain rate well below what producers can
 * fill, so the ring runs full while the consumer sits in syscalls.  Batching
 * takes the syscall out of the per-message cost.
 */
#define LOG_BATCH_SIZE	(64 * 1024)

struct log_batch {
	size_t n;
	char   b[LOG_BATCH_SIZE];
};

static void batch_flush(int fd, struct log_batch *w)
{
	if (w->n && fd >= 0)
		write_all(fd, w->b, (uint32_t)w->n);
	w->n = 0;
}

/* Route each payload to its file.  Nothing is formatted here: a record costs
 * one memcpy and its share of a batched write. */
static void drain(struct unvmed_log_ring *r, struct log_batch *text,
		  struct log_batch *trace)
{
	char     rec[UNVMED_LOG_MSG_SIZE];
	uint32_t len;
	uint8_t  type;

	for (;;) {
		if (!ring_pop(r, rec, &len, &type))
			return;

		if (type == UNVMED_LOG_REC_TEXT) {
			if (text->n + len > sizeof(text->b))
				batch_flush(r->fd, text);
			memcpy(text->b + text->n, rec, len);
			text->n += len;
		} else if (r->fd_trace >= 0) {
			if (trace->n + len > sizeof(trace->b))
				batch_flush(r->fd_trace, trace);
			memcpy(trace->b + trace->n, rec, len);
			trace->n += len;
		}
	}
}

/* Backstop for a producer that never finishes publishing, so a wedged or
 * stopped thread cannot hang shutdown for good.  usleep(1) rounds up to the
 * timer granularity, so this bounds the wait at seconds, not microseconds. */
#define LOG_QUIESCE_MAX_SPINS	100000

static void *logger_thread(void *arg)
{
	struct unvmed_log_ring *r = arg;
	struct log_batch *text, *trace;
	struct timespec ts;

	/* Heap, not static: one pair per ring, and too large for the stack. */
	text  = calloc(1, sizeof(*text));
	trace = calloc(1, sizeof(*trace));
	if (!text || !trace) {
		free(text);
		free(trace);
		return NULL;
	}

	while (atomic_load_explicit(&r->running, memory_order_acquire)) {
		drain(r, text, trace);

		/* Flush before sleeping: batching may hold lines back, but only
		 * while there is more to drain.  A log that goes quiet must not
		 * leave its last lines in the buffer. */
		batch_flush(r->fd, text);
		batch_flush(r->fd_trace, trace);

		/* The 1 ms timeout also catches a signal raced past the wait. */
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
	 * Clearing @running does not mean the ring is quiet: a producer that
	 * passed the check may still be between claiming a slot and publishing
	 * it.  Such a slot is a hole that stops ring_pop() dead and strands
	 * everything queued behind it, so drain until no claims are
	 * outstanding before the final pass.
	 */
	for (int i = 0; i < LOG_QUIESCE_MAX_SPINS; i++) {
		drain(r, text, trace);

		if (!atomic_load_explicit(&r->n_producers, memory_order_seq_cst))
			break;

		usleep(1);
	}

	drain(r, text, trace);
	batch_flush(r->fd, text);
	batch_flush(r->fd_trace, trace);

	free(text);
	free(trace);
	return NULL;
}

void unvmed_log_ring_set_lossless(struct unvmed_log_ring *r, bool lossless)
{
	atomic_store_explicit(&r->lossless, lossless, memory_order_relaxed);
}

int unvmed_log_ring_init(struct unvmed_log_ring *r, int fd, int fd_trace)
{
	int rc;

	/* Initialise every slot's sequence number to its own index so that
	 * slot[i] is immediately claimable by a producer whose write_pos
	 * modulo RING_SLOTS equals i. */
	for (unsigned i = 0; i < UNVMED_LOG_RING_SLOTS; i++)
		atomic_init(&r->slots[i].seq, (uint64_t)i);

	atomic_init(&r->write_pos, 0);
	atomic_init(&r->n_dropped, 0);
	atomic_init(&r->n_stalled, 0);
	atomic_init(&r->n_producers, 0);
	atomic_init(&r->running,   true);
	r->read_pos = 0;
	r->fd       = fd;
	r->fd_trace = fd_trace;

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
 * Producer side, called from the I/O threads.
 *
 * Claims a slot only once it is confirmed free, so abandoning the attempt
 * consumes no sequence number.  A full ring drops the payload, or waits for
 * space when the ring is in lossless mode.
 */
void unvmed_log_ring_push(struct unvmed_log_ring *r, uint8_t type,
			  const void *rec, uint32_t len)
{
	struct unvmed_log_slot *slot;
	uint64_t pos, seq;
	int64_t  dif;
	bool     stalled = false;

	/*
	 * Register before testing @running, both seq_cst.  This is the
	 * store-load half of the shutdown handshake: stop() stores
	 * running=false and the logger thread then waits for n_producers to
	 * reach zero.  Acquire/release would let each side be reordered past
	 * the other and miss it; seq_cst is what makes the guarantee hold,
	 * namely that a slot claimed here is drained before the logger exits.
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
			/*
			 * Ring full.  Waiting is safe here: no slot is
			 * claimed yet, so we block nobody.  Re-check
			 * @running so a stop during the wait cannot strand
			 * us.
			 */
			if (atomic_load_explicit(&r->lossless,
						 memory_order_relaxed) &&
			    atomic_load_explicit(&r->running,
						 memory_order_acquire)) {
				if (!stalled) {
					atomic_fetch_add_explicit(
						&r->n_stalled, 1,
						memory_order_relaxed);
					stalled = true;
				}
				sched_yield();
				pos = atomic_load_explicit(&r->write_pos,
							   memory_order_relaxed);
				continue;
			}

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

	/* The consumer uses exactly @len bytes and never treats the payload as
	 * a C string, so the whole slot is usable. */
	uint32_t n = (len < UNVMED_LOG_MSG_SIZE) ? len : UNVMED_LOG_MSG_SIZE;
	memcpy(slot->msg, rec, n);
	slot->len  = n;
	slot->type = type;

	/* Publish to consumer */
	atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);

	/* Only after publishing: until now the logger thread keeps waiting. */
	atomic_fetch_sub_explicit(&r->n_producers, 1, memory_order_release);

	/* trylock so the hot path never blocks: if the consumer holds the lock
	 * it is already awake and will pick this up. */
	if (!pthread_mutex_trylock(&r->lock)) {
		pthread_cond_signal(&r->cond);
		pthread_mutex_unlock(&r->lock);
	}
}

void unvmed_log_ring_stop(struct unvmed_log_ring *r)
{
	bool was_running;

	/*
	 * Idempotent.  seq_cst pairs with push(); see the handshake there.
	 * The matching wait on @n_producers lives in the logger thread, so
	 * pthread_join() below already covers it.
	 */
	was_running = atomic_exchange_explicit(&r->running, false,
					       memory_order_seq_cst);
	if (!was_running)
		return;

	/* Cannot join ourselves; @running is cleared, so the logger thread
	 * unwinds on its own. */
	if (pthread_equal(pthread_self(), r->thread))
		return;

	/* trylock: a signal handler may have interrupted a thread holding the
	 * lock.  On failure the thread is already awake and exits within the
	 * 1 ms timedwait. */
	if (!pthread_mutex_trylock(&r->lock)) {
		pthread_cond_signal(&r->cond);
		pthread_mutex_unlock(&r->lock);
	}

	pthread_join(r->thread, NULL);

	/* Report what the ring had to do under pressure. */
	uint64_t dropped = atomic_load_explicit(&r->n_dropped,
						memory_order_relaxed);
	uint64_t stalled = atomic_load_explicit(&r->n_stalled,
						memory_order_relaxed);
	if ((dropped || stalled) && r->fd >= 0) {
		char dmsg[128];
		int  dlen;

		if (dropped)
			dlen = snprintf(dmsg, sizeof(dmsg),
					"log: %llu message(s) dropped (ring full)\n",
					(unsigned long long)dropped);
		else
			dlen = snprintf(dmsg, sizeof(dmsg),
					"log: %llu push(es) waited for ring space "
					"(lossless mode, nothing dropped)\n",
					(unsigned long long)stalled);
		/* (size_t), not (uint32_t): the comparison must stay in the
		 * same type as sizeof so a truncating snprintf is detected. */
		if (dlen > 0 && (size_t)dlen < sizeof(dmsg))
			write_all(r->fd, dmsg, (uint32_t)dlen);
	}

	/*
	 * The mutex and condvar are deliberately not destroyed: a racing
	 * push() can still reach the trylock between its @running check and
	 * the lock.  They are freed when the process exits.
	 */
}
