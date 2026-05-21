// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
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
		 * Ring is full — this shouldn't happen in normal operation
		 * because the ring is sized queue_depth+1.  Log and skip;
		 * the kernel will eventually abort the request.
		 */
		unvmed_log_err("ublk q%d tag %u: no SQE for COMMIT_AND_FETCH",
			       q->qid, tag);
		return;
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

static int submit_nvme_io(struct unvmed_ublk_queue *q,
			  const struct ublksrv_io_desc *iod, uint16_t tag)
{
	struct unvmed_ublk_server *s = q->server;
	struct unvme *u = s->u;
	uint8_t  op       = ublksrv_get_op(iod);
	uint64_t slba     = iod->start_sector / (s->lba_size / 512);
	uint32_t nr_sects = iod->nr_sectors;
	uint16_t nlb      = (uint16_t)(nr_sects / (s->lba_size / 512)) - 1;
	void    *buf      = (char *)q->bounce + tag * q->slot_size;
	size_t   len      = (size_t)nr_sects * 512;
	struct unvme_cmd *cmd;
	struct iovec iov  = { .iov_base = buf, .iov_len = len };
	int ret = 0;

	if (op == UBLK_IO_OP_FLUSH ||
	    op == UBLK_IO_OP_DISCARD ||
	    op == UBLK_IO_OP_WRITE_ZEROES) {
		/*
		 * Flush / discard: complete immediately with success.
		 * A full implementation would issue NVMe Flush or DSM here.
		 */
		submit_commit_and_fetch(q, tag, 0);
		return 0;
	}

	if (op != UBLK_IO_OP_READ && op != UBLK_IO_OP_WRITE) {
		submit_commit_and_fetch(q, tag, -EIO);
		return 0;
	}

	/*
	 * unvmed_alloc_cmd() detects that `buf` is already IOMMU-mapped
	 * (via unvmed_map_vaddr in queue init) and does NOT re-map it,
	 * ensuring the bounce buffer remains stably mapped for the lifetime
	 * of the server.
	 */
	cmd = unvmed_alloc_cmd(u, q->usq, NULL, buf, len);
	if (!cmd) {
		unvmed_log_err("ublk q%d tag %u: failed to alloc NVMe cmd: %s",
			       q->qid, tag, strerror(errno));
		submit_commit_and_fetch(q, tag, -EIO);
		return -1;
	}

	/*
	 * Store the ublk tag in cmd->opaque so we can correlate the NVMe
	 * completion back to the ublk request.
	 */
	cmd->opaque = (void *)(uintptr_t)tag;
	q->inflight[tag] = cmd;

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
		q->inflight[tag] = NULL;
		submit_commit_and_fetch(q, tag, -EIO);
		return -1;
	}

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
 * Returns number of completions processed; completed ublk COMMIT SQEs have
 * been added to the ring but NOT yet submitted.
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
		nr = unvmed_cq_run_n(s->u, q->usq, q->ucq, NULL, cqes,
				     0, max);
		if (nr > 0)
			goto process;
	} while (now_us() < spin_deadline);

	/* Yield phase: give up the CPU, try one more time */
	sched_yield();
	nr = unvmed_cq_run_n(s->u, q->usq, q->ucq, NULL, cqes, 0, max);

process:
	for (int i = 0; i < nr; i++) {
		uint16_t cid    = le16_to_cpu(cqes[i].cid);
		uint16_t status = le16_to_cpu(cqes[i].sfp) >> 1;

		if (cid >= s->queue_depth || !q->inflight[cid])
			continue;

		struct unvme_cmd *cmd = q->inflight[cid];
		uint16_t tag = (uint16_t)(uintptr_t)cmd->opaque;
		int      result = status ? -EIO : 0;

		unvmed_cmd_put(cmd);
		q->inflight[cid] = NULL;

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
	struct io_uring_cqe        *ublk_cqes[UNVMED_UBLK_DEF_DEPTH * 2];
	struct nvme_cqe             nvme_cqes[UNVMED_UBLK_DEF_DEPTH];
	int                         nr_posted;
	bool                        fetch_signalled = false;

	unvmed_log_info("ublk q%d handler started (NVMe qid=%u)",
			q->qid, s->base_qid + (uint32_t)q->qid);

	/*
	 * Submit initial FETCH_REQs from this handler thread.
	 *
	 * io_uring task-work is per-thread: SQEs submitted by a thread are
	 * processed as task-work on that same thread's next io_uring_enter().
	 * prefill_fetch() runs on the main thread, so its FETCH_REQ task-work
	 * is pinned to the main thread.  The main thread then blocks in
	 * io_uring_wait_cqe() on the *ctrl* ring and never re-enters the
	 * per-queue ring, so the task-work never runs and ublk_mark_io_ready()
	 * is never called — START_DEV waits forever.
	 *
	 * By submitting FETCH_REQs here, the task-work is owned by this
	 * handler thread.  The first io_uring_submit_and_wait() below will
	 * flush it and invoke ublk_mark_io_ready() for each tag.
	 */
	for (uint32_t tag = 0; tag < s->queue_depth; tag++) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&q->ring);
		struct ublksrv_io_cmd *io_cmd;

		if (!sqe) {
			unvmed_log_err("ublk q%d: no SQE for initial FETCH tag %u",
				       q->qid, tag);
			q->running = false;
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

	while (q->running) {
		nr_posted = 0;

		/*
		 * ── Phase 1: submit pending SQEs and collect ublk CQEs ──
		 *
		 * io_uring_submit_and_wait(ring, 0) issues io_uring_enter(2)
		 * which flushes any pending task-work on this thread (including
		 * the initial FETCH_REQs prepared above).  wait_nr=0 means
		 * "submit and return immediately; don't wait for a CQE".
		 */
		io_uring_submit_and_wait(&q->ring, 0);

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

			if (res < 0) {
				/* FETCH_REQ failed (device stopping) */
				if (res == UBLK_IO_RES_ABORT)
					q->running = false;
				continue;
			}

			/* Read io_desc filled in by kernel */
			const struct ublksrv_io_desc *iod = &q->io_descs[tag];
			int r = submit_nvme_io(q, iod, tag);
			if (r > 0)
				nr_posted++;
		}

		/* Flush doorbell for all batched NVMe submissions */
		if (nr_posted > 0) {
			unvmed_sq_enter(q->usq);
			unvmed_sq_update_tail(s->u, q->usq);
			unvmed_sq_exit(q->usq);
		}

		/* ── Phase 2: hybrid-poll NVMe completions ── */
		int nr_nvme = poll_nvme_completions(q, nvme_cqes,
						    (int)s->queue_depth);

		/*
		 * COMMIT_AND_FETCH SQEs were added to the ring during phase 2;
		 * they will be submitted at the top of the next iteration via
		 * io_uring_submit_and_wait(), so no extra submit needed here.
		 */

		/* ── Phase 3: idle → yield ── */
		if (nr_ublk == 0 && nr_nvme == 0)
			sched_yield();
	}

	unvmed_log_info("ublk q%d handler stopped", q->qid);
	return NULL;
}
