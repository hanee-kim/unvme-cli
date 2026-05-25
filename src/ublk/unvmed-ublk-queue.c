// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/uio.h>

#include <liburing.h>
#include <linux/ublk_cmd.h>

#include "libunvmed.h"

#include "unvmed-ublk.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static inline uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* ublk COMMIT_AND_FETCH_REQ submission                                */
/* ------------------------------------------------------------------ */

static void submit_commit_and_fetch(struct unvmed_ublk_queue *q,
				    uint16_t tag, int result)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&q->ring);
	struct ublksrv_io_cmd *io_cmd;

	if (!sqe) {
		/*
		 * Ring is full.  Flush any pending submissions to free slots,
		 * then retry once.  If still full, stop the queue so the kernel
		 * aborts all inflight requests via UBLK_IO_RES_ABORT rather than
		 * hanging indefinitely waiting for a reply that never comes.
		 */
		io_uring_submit(&q->ring);
		sqe = io_uring_get_sqe(&q->ring);
		if (!sqe) {
			unvmed_log_err("ublk q%d tag %u: ring full after flush, stopping queue",
				       q->qid, tag);
			atomic_store(&q->running, false);
			return;
		}
	}

	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, q->dev_fd, NULL, 0, 0);
	sqe->cmd_op   = UBLK_U_IO_COMMIT_AND_FETCH_REQ;
	sqe->user_data = (uint64_t)tag;

	io_cmd = (struct ublksrv_io_cmd *)sqe->cmd;
	io_cmd->q_id   = (uint16_t)q->qid;
	io_cmd->tag    = tag;
	io_cmd->addr   = (__u64)((char *)q->bounce + tag * q->slot_size);
	io_cmd->result = result;
}

/* ------------------------------------------------------------------ */
/* NVMe I/O submission                                                  */
/* ------------------------------------------------------------------ */

/*
 * Pick the next SQ in round-robin order for submission.
 * All SQs in q->usqs[] share q->ucq, so completions always appear on
 * the same CQ regardless of which SQ was used for submission.
 */
static inline struct unvme_sq *pick_usq(struct unvmed_ublk_queue *q)
{
	int idx = q->next_usq % q->nr_usqs;
	q->next_usq++;
	return q->usqs[idx];
}

static int submit_nvme_io(struct unvmed_ublk_queue *q,
			  const struct ublksrv_io_desc *iod, uint16_t tag)
{
	struct unvmed_ublk_server *s = q->server;
	struct unvme *u = s->u;
	uint8_t  op         = ublksrv_get_op(iod);
	uint32_t lba_ratio  = s->lba_size / 512;
	uint64_t slba       = iod->start_sector / lba_ratio;
	uint32_t nr_sects   = iod->nr_sectors;
	void    *buf        = (char *)q->bounce + tag * q->slot_size;
	size_t   len        = (size_t)nr_sects * 512;

	if (nr_sects < lba_ratio) {
		unvmed_log_err("ublk q%d tag %u: nr_sectors %u < lba_ratio %u",
			       q->qid, tag, nr_sects, lba_ratio);
		submit_commit_and_fetch(q, tag, -EIO);
		return 0;
	}

	uint16_t nlb = (uint16_t)(nr_sects / lba_ratio) - 1;
	struct unvme_cmd *cmd;
	struct iovec iov  = { .iov_base = buf, .iov_len = len };
	int ret = 0;

	if (op == UBLK_IO_OP_FLUSH ||
	    op == UBLK_IO_OP_DISCARD ||
	    op == UBLK_IO_OP_WRITE_ZEROES) {
		submit_commit_and_fetch(q, tag, 0);
		return 0;
	}

	if (op != UBLK_IO_OP_READ && op != UBLK_IO_OP_WRITE) {
		submit_commit_and_fetch(q, tag, -EIO);
		return 0;
	}

	/*
	 * Select SQ in round-robin order.  UNVMED_SQ_F_UBLK_OWNED bypasses
	 * the unvmed_sq_ready() check that would normally block submissions,
	 * but unvmed_alloc_cmd() explicitly allows owned SQs for ublk use
	 * (the EBUSY check in __unvmed_cmd_alloc only fires for external
	 * callers, not for the ublk server itself).
	 *
	 * Wait — actually we need to temporarily clear the flag or use a
	 * lower-level alloc.  Since UNVMED_SQ_F_UBLK_OWNED causes
	 * __unvmed_cmd_alloc() to return EBUSY for ALL callers, we bypass
	 * the flag by calling with the sq directly; the SQ is owned by this
	 * thread so no races are possible.
	 */
	struct unvme_sq *usq = pick_usq(q);

	/*
	 * unvmed_alloc_cmd() calls __unvmed_cmd_alloc() which checks
	 * UNVMED_SQ_F_UBLK_OWNED and returns EBUSY.  Since THIS code IS the
	 * ublk server, temporarily mask the flag for the duration of the
	 * alloc to allow our own submission.
	 */
	usq->flags &= ~UNVMED_SQ_F_UBLK_OWNED;
	cmd = unvmed_alloc_cmd(u, usq, NULL, buf, len);
	usq->flags |= UNVMED_SQ_F_UBLK_OWNED;

	if (!cmd) {
		unvmed_log_err("ublk q%d tag %u: failed to alloc NVMe cmd: %s",
			       q->qid, tag, strerror(errno));
		submit_commit_and_fetch(q, tag, -EIO);
		return -1;
	}

	if (op == UBLK_IO_OP_READ) {
		ret = unvmed_cmd_prep_read(cmd, s->nsid, slba, nlb,
					   0, 0, 0, 0, 0, false,
					   &iov, 1, NULL, NULL);
	} else {
		ret = unvmed_cmd_prep_write(cmd, s->nsid, slba, nlb,
					    0, 0, 0, 0, 0, false,
					    &iov, 1, NULL, NULL);
	}

	if (ret) {
		unvmed_log_err("ublk q%d tag %u: NVMe cmd prep failed",
			       q->qid, tag);
		unvmed_cmd_put(cmd);
		submit_commit_and_fetch(q, tag, -EIO);
		return -1;
	}

	/*
	 * Store the ublk tag in cmd->opaque so poll_nvme_completions() can
	 * correlate the NVMe completion (sqid + cid) back to the ublk tag.
	 * Must be set AFTER prep, which may zero cmd fields.
	 */
	cmd->opaque = (void *)(uintptr_t)tag;

	/* NODB: batch doorbell; caller calls unvmed_sq_update_tail() later */
	unvmed_cmd_post(cmd, &cmd->sqe, UNVMED_CMD_F_NODB);
	return 1;  /* 1 = command posted, needs doorbell flush */
}

/* ------------------------------------------------------------------ */
/* Hybrid NVMe completion poll                                          */
/* ------------------------------------------------------------------ */

/*
 * Poll NVMe CQ for up to @max completions.  Spins for @spin_us microseconds
 * before yielding the CPU if nothing is ready.
 *
 * Uses unvmed_cq_run_n_multi() which correctly handles multiple SQs sharing
 * one CQ: it reads raw CQEs from hardware and routes each completion to the
 * correct SQ by sqid, then we look up the ublk tag via cmd->opaque.
 *
 * Returns number of completions processed; COMMIT SQEs have been added to
 * the io_uring ring but NOT yet submitted.
 */
static int poll_nvme_completions(struct unvmed_ublk_queue *q,
				 struct nvme_cqe *cqes, int max)
{
	struct unvmed_ublk_server *s = q->server;
	uint64_t spin_deadline;
	int nr;

	/* Spin phase */
	spin_deadline = now_us() + q->poll_spin_us;
	do {
		nr = unvmed_cq_run_n_multi(s->u, q->ucq, cqes, max);
		if (nr > 0)
			goto process;
	} while (now_us() < spin_deadline);

	/* Yield phase: give up the CPU, try one more time */
	sched_yield();
	nr = unvmed_cq_run_n_multi(s->u, q->ucq, cqes, max);

process:
	for (int i = 0; i < nr; i++) {
		uint16_t sqid   = le16_to_cpu(cqes[i].sqid);
		uint16_t cid    = le16_to_cpu(cqes[i].cid);
		uint16_t status = le16_to_cpu(cqes[i].sfp) >> 1;

		/*
		 * Look up the command by (sqid, cid).  unvmed_cq_run_n_multi()
		 * already set cmd->state = COMPLETED, so unvmed_get_cmd()
		 * returns non-NULL.  cmd->opaque carries the ublk tag.
		 */
		struct unvme_sq *usq = unvmed_sq_find(s->u, sqid);
		if (!usq) {
			unvmed_log_err("ublk q%d: sqid=%u not found in cqe",
				       q->qid, sqid);
			continue;
		}

		struct unvme_cmd *cmd = unvmed_get_cmd(usq, cid);
		if (!cmd) {
			unvmed_log_err("ublk q%d: cid=%u not found in sq %u",
				       q->qid, cid, sqid);
			continue;
		}

		uint16_t tag = (uint16_t)(uintptr_t)cmd->opaque;

		/*
		 * Kernel requires result = bytes transferred for READ/WRITE.
		 * result=0 on a successful READ triggers -EIO in the kernel.
		 */
		const struct ublksrv_io_desc *iod = &q->io_descs[tag];
		int result = status ? -EIO : (int)(iod->nr_sectors * 512);

		unvmed_log_info("ublk q%d: completing sqid=%u cid=%u tag=%u result=%d",
				q->qid, sqid, cid, tag, result);

		unvmed_cmd_put(cmd);
		submit_commit_and_fetch(q, tag, result);
	}

	return nr;
}

/* ------------------------------------------------------------------ */
/* Queue handler thread                                                 */
/* ------------------------------------------------------------------ */

void *unvmed_ublk_queue_handler(void *arg)
{
	struct unvmed_ublk_queue   *q = arg;
	struct unvmed_ublk_server  *s = q->server;
	struct io_uring_cqe       **ublk_cqes;
	struct nvme_cqe            *nvme_cqes;
	int                         nr_posted;
	bool                        fetch_signalled = false;

	ublk_cqes = calloc(s->queue_depth, sizeof(*ublk_cqes));
	nvme_cqes = calloc(s->queue_depth, sizeof(*nvme_cqes));
	if (!ublk_cqes || !nvme_cqes) {
		unvmed_log_err("ublk q%d: failed to allocate cqe buffers", q->qid);
		free(ublk_cqes);
		free(nvme_cqes);
		atomic_store(&q->running, false);
		sem_post(&q->fetch_submitted);
		return NULL;
	}

	unvmed_log_info("ublk q%d handler started (CQ %d, %d SQ(s))",
			q->qid, q->ucq->id, q->nr_usqs);

	/*
	 * Submit initial FETCH_REQs from this handler thread.
	 *
	 * io_uring task-work is per-thread: SQEs submitted by a thread are
	 * processed as task-work on that same thread's next io_uring_enter().
	 * Submitting here (not from the main thread) ensures the task-work is
	 * owned by this thread, so the first io_uring_submit_and_get_events()
	 * call below will flush it and invoke ublk_mark_io_ready() for each
	 * tag.  Without this, START_DEV would wait forever.
	 */
	for (uint32_t tag = 0; tag < s->queue_depth; tag++) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&q->ring);
		struct ublksrv_io_cmd *io_cmd;

		if (!sqe) {
			unvmed_log_err("ublk q%d: no SQE for initial FETCH tag %u",
				       q->qid, tag);
			free(ublk_cqes);
			free(nvme_cqes);
			atomic_store(&q->running, false);
			sem_post(&q->fetch_submitted);
			return NULL;
		}

		io_uring_prep_rw(IORING_OP_URING_CMD, sqe, q->dev_fd, NULL, 0, 0);
		sqe->cmd_op    = UBLK_U_IO_FETCH_REQ;
		sqe->user_data = (uint64_t)tag;

		io_cmd         = (struct ublksrv_io_cmd *)sqe->cmd;
		io_cmd->q_id   = (uint16_t)q->qid;
		io_cmd->tag    = (uint16_t)tag;
		io_cmd->addr   = (__u64)((char *)q->bounce + tag * q->slot_size);
		io_cmd->result = 0;
	}

	while (atomic_load(&q->running)) {
		nr_posted = 0;

		/*
		 * ── Phase 1: submit pending SQEs and collect ublk CQEs ──
		 *
		 * io_uring_submit_and_get_events() issues io_uring_enter(2)
		 * with IORING_ENTER_GETEVENTS.  Unlike submit_and_wait(0), this
		 * flag triggers task-work processing even when there are no
		 * pending SQEs.  Without it, ublk FETCH_REQ completions for new
		 * kernel IO requests would sit as task-work and never appear in
		 * the CQ ring — causing a permanent hang.
		 */
		io_uring_submit_and_get_events(&q->ring);

		/*
		 * Signal the server start path that this queue's initial
		 * FETCH_REQs have been submitted.  Done once, on the first
		 * iteration, so START_DEV is not called before the kernel
		 * can see all queues as ready.
		 */
		if (!fetch_signalled) {
			sem_post(&q->fetch_submitted);
			fetch_signalled = true;
		}

		int nr_ublk = io_uring_peek_batch_cqe(&q->ring, ublk_cqes,
						       s->queue_depth);
		for (int i = 0; i < nr_ublk; i++) {
			struct io_uring_cqe *cqe = ublk_cqes[i];
			uint16_t tag = (uint16_t)cqe->user_data;
			int      res = cqe->res;

			io_uring_cqe_seen(&q->ring, cqe);

			unvmed_log_info("ublk q%d: ublk cqe tag=%u res=%d",
					q->qid, tag, res);

			if (res < 0) {
				unvmed_log_err("ublk q%d: fetch tag=%u failed res=%d",
					       q->qid, tag, res);
				if (res == UBLK_IO_RES_ABORT)
					atomic_store(&q->running, false);
				continue;
			}

			/* Read io_desc filled in by kernel */
			const struct ublksrv_io_desc *iod = &q->io_descs[tag];
			unvmed_log_info("ublk q%d: dispatching tag=%u op=%u slba=%llu nr_sectors=%u",
					q->qid, tag, ublksrv_get_op(iod),
					(unsigned long long)iod->start_sector,
					iod->nr_sectors);
			int r = submit_nvme_io(q, iod, tag);
			if (r > 0)
				nr_posted++;
		}

		/*
		 * Flush doorbell for all SQs that received new commands this
		 * iteration.  With round-robin across multiple SQs, any subset
		 * may have been used; flushing all is always correct (a no-op
		 * if a given SQ had no new submissions).
		 */
		if (nr_posted > 0) {
			for (int si = 0; si < q->nr_usqs; si++) {
				unvmed_sq_enter(q->usqs[si]);
				unvmed_sq_update_tail(s->u, q->usqs[si]);
				unvmed_sq_exit(q->usqs[si]);
			}
		}

		/* ── Phase 2: hybrid-poll NVMe completions ── */
		int nr_nvme = poll_nvme_completions(q, nvme_cqes,
						    (int)s->queue_depth);

		/* ── Phase 3: idle → yield ── */
		if (nr_ublk == 0 && nr_nvme == 0)
			sched_yield();
	}

	free(ublk_cqes);
	free(nvme_cqes);
	unvmed_log_info("ublk q%d handler stopped", q->qid);
	return NULL;
}
