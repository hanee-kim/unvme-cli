// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/uio.h>

#include <liburing.h>
#include <linux/ublk_cmd.h>

#include "libunvmed.h"

#include "unvmed-ublk.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

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
				       q->ucq->id, tag);
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


static int submit_nvme_io(struct unvmed_ublk_queue *q,
			  const struct ublksrv_io_desc *iod, uint16_t tag)
{
	struct unvmed_ublk_server *s = q->server;
	struct unvme *u = s->u;
	uint8_t  op         = ublksrv_get_op(iod);
	uint32_t lba_ratio  = s->lba_size / 512;
	uint64_t slba       = iod->start_sector / lba_ratio;
	uint32_t nr_sects   = iod->nr_sectors;
	if (nr_sects < lba_ratio) {
		unvmed_log_err("ublk q%d tag %u: nr_sectors %u < lba_ratio %u",
			       q->ucq->id, tag, nr_sects, lba_ratio);
		submit_commit_and_fetch(q, tag, -EIO);
		return 0;
	}

	uint16_t nlb = (uint16_t)(nr_sects / lba_ratio) - 1;
	struct unvme_cmd *cmd;

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

	struct unvme_sq *usq = q->usq;

	usq->flags &= ~UNVMED_SQ_F_UBLK_OWNED;
	cmd = unvmed_alloc_cmd_nodata(u, usq, NULL);
	usq->flags |= UNVMED_SQ_F_UBLK_OWNED;

	if (!cmd) {
		unvmed_log_err("ublk q%d tag %u: failed to alloc NVMe cmd: %s",
			       q->ucq->id, tag, strerror(errno));
		submit_commit_and_fetch(q, tag, -EIO);
		return -1;
	}

	/*
	 * Build the NVMe R/W SQE directly using the pre-cached bounce IOVA,
	 * bypassing iommu_translate_vaddr() (skiplist + rwlock) that
	 * unvmed_alloc_cmd(buf) and cmd_prep_read/write() would invoke on
	 * every I/O.  4K I/O fits in a single page so prp2 is unused.
	 */
	{
		struct nvme_cmd_rw *sqe = (struct nvme_cmd_rw *)&cmd->sqe;
		uint64_t iova = q->bounce_iova + (uint64_t)tag * q->slot_size;

		sqe->opcode = (op == UBLK_IO_OP_READ) ? 0x02 : 0x01;
		sqe->nsid   = cpu_to_le32(s->nsid);
		sqe->slba   = cpu_to_le64(slba);
		sqe->nlb    = cpu_to_le16(nlb);
		sqe->cid    = cmd->cid;
		sqe->dptr.prp1 = cpu_to_le64(iova);
		sqe->dptr.prp2 = 0;
	}

	cmd->opaque = (void *)(uintptr_t)tag;

	/*
	 * Post directly to the NVMe SQ, bypassing unvmed_cmd_post() which
	 * does a global atomic CAS on u->nr_cmds — a contention point when
	 * multiple queues submit simultaneously.  Doorbell is batched by
	 * the caller via unvmed_sq_update_tail().
	 */
	atomic_store_release(&cmd->state, UNVME_CMD_S_SUBMITTED);
	nvme_sq_post(usq->q, (union nvme_cmd *)&cmd->sqe);
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
	int nr;

	nr = unvmed_cq_run_n_multi(s->u, q->ucq, cqes, max);

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
				       q->ucq->id, sqid);
			continue;
		}

		struct unvme_cmd *cmd = unvmed_get_cmd(usq, cid);
		if (!cmd) {
			unvmed_log_err("ublk q%d: cid=%u not found in sq %u",
				       q->ucq->id, cid, sqid);
			continue;
		}

		uint16_t tag = (uint16_t)(uintptr_t)cmd->opaque;

		/*
		 * Kernel requires result = bytes transferred for READ/WRITE.
		 * result=0 on a successful READ triggers -EIO in the kernel.
		 */
		const struct ublksrv_io_desc *iod = &q->io_descs[tag];
		int result = status ? -EIO : (int)(iod->nr_sectors * 512);

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
		unvmed_log_err("ublk q%d: failed to allocate cqe buffers", q->ucq->id);
		free(ublk_cqes);
		free(nvme_cqes);
		atomic_store(&q->running, false);
		sem_post(&q->fetch_submitted);
		return NULL;
	}

	unvmed_log_info("ublk q%d handler started", q->ucq->id);

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
				       q->ucq->id, tag);
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

			if (res == UBLK_IO_RES_ABORT) {
				atomic_store(&q->running, false);
				continue;
			}
			if (res < 0) {
				unvmed_log_err("ublk q%d: fetch tag=%u failed res=%d",
					       q->ucq->id, tag, res);
				continue;
			}

			/* Read io_desc filled in by kernel */
			const struct ublksrv_io_desc *iod = &q->io_descs[tag];
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
			unvmed_sq_enter(q->usq);
			unvmed_sq_update_tail(s->u, q->usq);
			unvmed_sq_exit(q->usq);
		}

		/* ── Phase 2: poll NVMe completions ── */
		poll_nvme_completions(q, nvme_cqes, (int)s->queue_depth);

		/* ── Phase 3: busy-poll, no yield ── */
	}

	free(ublk_cqes);
	free(nvme_cqes);
	unvmed_log_info("ublk q%d handler stopped", q->ucq->id);
	return NULL;
}
