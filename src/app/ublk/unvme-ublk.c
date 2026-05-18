// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ublk server backed by libunvmed.
 *
 * Architecture overview:
 *
 *   Multiple fio/app processes → /dev/ublkb<N> (standard block device)
 *        ↕  kernel block layer (bio fan-in, scheduling)
 *   ublk kernel driver  (/dev/ublkc<N>)
 *        ↕  io_uring URING_CMD  [one ring per queue_thread]
 *   ┌──────────────────────────────────────────────────────┐
 *   │  queue_thread (per queue)                             │
 *   │    io_uring loop:                                     │
 *   │      FETCH_REQ CQE → submit NVMe I/O via libunvmed   │
 *   │      eventfd CQE  ← NVMe completion from poller      │
 *   │         → drain SPSC ring → COMMIT_AND_FETCH_REQ     │
 *   └──────────────────────────────────────────────────────┘
 *        ↕  eventfd (write) + SPSC ring
 *   ┌──────────────────────────────────────────────────────┐
 *   │  poller_thread (per queue)                            │
 *   │    tight loop: unvmed_cq_run()                        │
 *   │      on CQE: pwrite READ data back to ublkc if READ  │
 *   │              push (tag, result) to SPSC ring          │
 *   │              write(eventfd)                           │
 *   └──────────────────────────────────────────────────────┘
 *        ↕  libunvmed (unvmed_cmd_post / unvmed_cq_run)
 *   unvmed daemon → NVMe HW via VFIO
 *
 * Data path (UBLK_F_USER_COPY):
 *   WRITE: queue_thread pread()s request data from /dev/ublkc<N>
 *          into a pre-allocated IOMMU-mapped DMA buffer, then submits
 *          a NVMe Write.
 *   READ:  queue_thread submits a NVMe Read into the DMA buffer;
 *          poller_thread pwrite()s the result back to /dev/ublkc<N>
 *          before signalling completion.
 *
 * CID mapping:
 *   After unvmed_alloc_cmd() assigns a CID, we store it in
 *   cid_to_tag[cid] so the poller_thread can reconstruct the ublk
 *   tag from the NVMe CQE's CID field.  We also store the cmd pointer
 *   in pending_cmds[tag] with a release barrier so the poller_thread
 *   can retrieve it without a lock.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <linux/ublk_cmd.h>
#include <liburing.h>

#undef cpu_to_le64
#undef cpu_to_be64
#undef cpu_to_le32
#undef cpu_to_be32
#undef cpu_to_le16
#undef cpu_to_be16
#include <nvme/types.h>
#include <vfn/nvme.h>

#include "libunvmed.h"
#include "unvme-ublk.h"

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define UBLK_CTRL_DEV		"/dev/ublk-control"
#define UBLK_CDEV_FMT		"/dev/ublkc%d"
#define UBLK_BDEV_FMT		"/dev/ublkb%d"

/* Linux block layer uses 512-byte sectors regardless of LBA size */
#define UBLK_SECTOR_SHIFT	9u
#define UBLK_SECTOR_SIZE	(1u << UBLK_SECTOR_SHIFT)

/*
 * Sentinel in io_uring user_data to distinguish eventfd POLL_ADD CQEs
 * from FETCH_REQ / COMMIT_AND_FETCH CQEs (whose user_data == tag ≤ 4095).
 */
#define UD_EFD_COOKIE		0x10000ULL

/* SPSC ring: must be a power of two and ≥ UBLK_MAX_QUEUE_DEPTH (4096) */
#define COMP_RING_SIZE		4096u
#define COMP_RING_MASK		(COMP_RING_SIZE - 1u)

#define MAX_UBLK_SERVERS	16

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

struct comp_entry {
	uint16_t tag;
	int32_t  result;
};

struct ublk_queue {
	/* back-pointer */
	struct unvme_ublk_server *server;

	int		 dev_id;
	int		 qid;
	int		 cdev_fd;
	struct io_uring	 ring;

	/*
	 * eventfd: poller_thread writes here when NVMe completions are
	 * available; queue_thread polls it via IORING_OP_POLL_ADD.
	 */
	int		 efd;
	volatile bool	 running;

	/* libunvmed handles for this queue's SQ/CQ pair */
	struct unvme	*u;
	struct unvme_sq	*usq;
	struct unvme_cq	*ucq;

	uint32_t	 depth;
	uint32_t	 max_io_buf_bytes;
	uint8_t		 lba_shift;
	uint32_t	 nsid;

	/*
	 * DMA buffer pool: one contiguous, page-aligned, IOMMU-mapped
	 * allocation.  Tag T's buffer starts at dma_pool + T*max_io_buf_bytes.
	 */
	void		*dma_pool;

	/*
	 * pending_cmds[tag]: cmd pointer stored by queue_thread after
	 * unvmed_alloc_cmd(), read by poller_thread after NVMe CQE.
	 * Written with __ATOMIC_RELEASE, read with __ATOMIC_ACQUIRE.
	 */
	struct unvme_cmd **pending_cmds;

	/*
	 * cid_to_tag[cid]: maps NVMe CID → ublk tag so that poller_thread
	 * can recover the tag from cqe.cid without a lock.
	 * Indexed by the CID assigned by unvmed_alloc_cmd(); CIDs are in
	 * [0, depth-1] because the SQ was created with size == depth.
	 */
	uint16_t	*cid_to_tag;

	/* SPSC completion ring: poller writes, queue_thread reads */
	struct comp_entry comp_ring[COMP_RING_SIZE];
	atomic_uint	  comp_head;	/* consumer (queue_thread) index */
	atomic_uint	  comp_tail;	/* producer (poller_thread) index */
};

struct unvme_ublk_server {
	int		 ctrl_fd;
	int		 dev_id;
	int		 nr_queues;
	uint32_t	 queue_depth;

	/* mmap'd io_desc shared memory: io_descs[qid][tag] */
	struct ublksrv_io_desc **io_descs;

	volatile bool	 running;

	/*
	 * Counts queue threads that have submitted their initial FETCH_REQs.
	 * The main thread spins on this before calling START_DEV, because the
	 * kernel blocks START_DEV until every queue has at least one pending
	 * FETCH_REQ in its io_uring.
	 */
	atomic_int	 queues_ready;

	struct unvme	*u;
	uint32_t	 nsid;
	int		 start_sqid;

	struct ublk_queue  *queues;
	pthread_t	   *queue_threads;
	pthread_t	   *poller_threads;
};

/* Global server table, protected by g_servers_mutex for start/stop */
static struct unvme_ublk_server *g_servers[MAX_UBLK_SERVERS];
static pthread_mutex_t g_servers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * ublk control helpers  (io_uring URING_CMD on /dev/ublk-control)
 *
 * Kernel 6.2+ removed the ioctl() handler from /dev/ublk-control; all
 * control commands (ADD_DEV, SET_PARAMS, START_DEV, …) must be issued via
 * io_uring IORING_OP_URING_CMD.  struct ublksrv_ctrl_cmd (32 bytes) is
 * passed as the SQE inline command, which requires IORING_SETUP_SQE128
 * (128-byte SQEs, giving 64 bytes for sqe->cmd[]).
 * ---------------------------------------------------------------------- */

static int ctrl_uring_cmd(int ctrl_fd, unsigned int cmd_op,
			   const struct ublksrv_ctrl_cmd *param)
{
	struct io_uring_params p = { .flags = IORING_SETUP_SQE128 };
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	ret = io_uring_queue_init_params(2, &ring, &p);
	if (ret)
		return ret;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, ctrl_fd, NULL, 0, 0);
	sqe->cmd_op = cmd_op;
	memcpy((void *)sqe->cmd, param, sizeof(*param));
	sqe->user_data = 1;

	ret = io_uring_submit(&ring);
	if (ret > 0) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret == 0) {
			ret = cqe->res;
			io_uring_cqe_seen(&ring, cqe);
		}
	} else {
		ret = ret < 0 ? ret : -EIO;
	}

	io_uring_queue_exit(&ring);
	return ret;
}

static int ublk_add_dev(int ctrl_fd, int dev_id, int nr_queues,
			uint32_t queue_depth, uint32_t max_io_buf_bytes)
{
	struct ublksrv_ctrl_dev_info info = {
		.nr_hw_queues	 = nr_queues,
		.queue_depth	 = queue_depth,
		.max_io_buf_bytes = max_io_buf_bytes,
		.dev_id		 = dev_id,
		.ublksrv_pid	 = getpid(),
		/*
		 * UBLK_F_USER_COPY: data transfer via pread()/pwrite() on the
		 *   character device; kernel does not manage the I/O buffer.
		 * UBLK_F_CMD_IOCTL_ENCODE: io_uring commands use the ioctl-
		 *   encoded UBLK_U_IO_* numbers so the kernel can validate sizes.
		 */
		.flags = UBLK_F_USER_COPY | UBLK_F_CMD_IOCTL_ENCODE,
	};
	struct ublksrv_ctrl_cmd param = {
		.dev_id	  = dev_id,
		.queue_id = (uint16_t)-1,
		.len	  = sizeof(info),
		.addr	  = (__u64)(uintptr_t)&info,
	};
	return ctrl_uring_cmd(ctrl_fd, UBLK_U_CMD_ADD_DEV, &param);
}

static int ublk_set_params(int ctrl_fd, int dev_id,
			   uint64_t nr_sectors, uint8_t lba_shift,
			   uint32_t max_sectors)
{
	uint8_t pbs = (lba_shift < 12) ? 12 : lba_shift;
	struct ublk_params params = {
		.len   = sizeof(params),
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift  = lba_shift,
			.physical_bs_shift = pbs,
			.io_opt_shift	   = pbs,
			.io_min_shift	   = lba_shift,
			.max_sectors	   = max_sectors,
			.dev_sectors	   = nr_sectors,
		},
	};
	struct ublksrv_ctrl_cmd param = {
		.dev_id	  = dev_id,
		.queue_id = (uint16_t)-1,
		.len	  = sizeof(params),
		.addr	  = (__u64)(uintptr_t)&params,
	};
	return ctrl_uring_cmd(ctrl_fd, UBLK_U_CMD_SET_PARAMS, &param);
}

static int ublk_start_dev(int ctrl_fd, int dev_id)
{
	struct ublksrv_ctrl_cmd param = {
		.dev_id	  = dev_id,
		.queue_id = (uint16_t)-1,
		.data[0]  = getpid(),	/* daemon PID required by kernel */
	};
	return ctrl_uring_cmd(ctrl_fd, UBLK_U_CMD_START_DEV, &param);
}

static int ublk_stop_dev(int ctrl_fd, int dev_id)
{
	struct ublksrv_ctrl_cmd param = {
		.dev_id	  = dev_id,
		.queue_id = (uint16_t)-1,
	};
	return ctrl_uring_cmd(ctrl_fd, UBLK_U_CMD_STOP_DEV, &param);
}

static int ublk_del_dev(int ctrl_fd, int dev_id)
{
	struct ublksrv_ctrl_cmd param = {
		.dev_id	  = dev_id,
		.queue_id = (uint16_t)-1,
	};
	return ctrl_uring_cmd(ctrl_fd, UBLK_U_CMD_DEL_DEV, &param);
}

/* -------------------------------------------------------------------------
 * io_uring SQE helpers for ublk I/O commands on /dev/ublkc<N>
 *
 * ublksrv_io_cmd (16 bytes) is placed in sqe->cmd[] which starts at
 * byte 64 of the SQE layout.  This requires IORING_SETUP_SQE128 (128-byte
 * SQEs); without it, sqe->cmd has zero space and writing there corrupts
 * the adjacent SQE slot.  sqe->cmd_op carries UBLK_U_IO_*.
 * ---------------------------------------------------------------------- */

static void prep_fetch_req(struct io_uring_sqe *sqe,
			   int cdev_fd, uint16_t qid, uint16_t tag)
{
	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode  = IORING_OP_URING_CMD;
	sqe->fd	     = cdev_fd;
	sqe->cmd_op  = UBLK_U_IO_FETCH_REQ;	/* in union with sqe->off */
	sqe->user_data = (uint64_t)tag;

	struct ublksrv_io_cmd *ioc = (struct ublksrv_io_cmd *)sqe->cmd;
	ioc->q_id   = qid;
	ioc->tag    = tag;
	ioc->result = 0;
	ioc->addr   = 0;	/* ignored with UBLK_F_USER_COPY */
}

static void prep_commit_and_fetch(struct io_uring_sqe *sqe,
				  int cdev_fd, uint16_t qid, uint16_t tag,
				  int32_t result)
{
	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode  = IORING_OP_URING_CMD;
	sqe->fd	     = cdev_fd;
	sqe->cmd_op  = UBLK_U_IO_COMMIT_AND_FETCH_REQ;
	sqe->user_data = (uint64_t)tag;

	struct ublksrv_io_cmd *ioc = (struct ublksrv_io_cmd *)sqe->cmd;
	ioc->q_id   = qid;
	ioc->tag    = tag;
	ioc->result = result;
	ioc->addr   = 0;
}

static void prep_poll_efd(struct io_uring_sqe *sqe, int efd)
{
	io_uring_prep_poll_add(sqe, efd, POLLIN);
	sqe->user_data = UD_EFD_COOKIE;
}

/* -------------------------------------------------------------------------
 * NVMe I/O submission  (called from queue_thread)
 *
 * Translates a ublk I/O descriptor into a NVMe command.  For WRITE, data
 * is first fetched from the kernel via pread() on the character device.
 * The cmd pointer and cid→tag mapping are stored for the poller_thread.
 * ---------------------------------------------------------------------- */

static int submit_nvme_io(struct ublk_queue *q, uint16_t tag)
{
	const struct ublksrv_io_desc *iod =
		&q->server->io_descs[q->qid][tag];
	uint8_t op = ublksrv_get_op(iod);

	/*
	 * Linux sectors are 512 bytes.  Convert to NVMe LBAs:
	 *   slba = start_sector >> (lba_shift - UBLK_SECTOR_SHIFT)
	 *   nlb  = nr_sectors   >> (lba_shift - UBLK_SECTOR_SHIFT)
	 * When lba_shift == 9 (512 B/LBA) the shifts are zero.
	 */
	uint32_t shift   = q->lba_shift - UBLK_SECTOR_SHIFT;
	uint64_t slba    = iod->start_sector >> shift;
	uint32_t nlb_cnt = iod->nr_sectors >> shift;	/* LBA count */
	size_t	 len	 = (size_t)iod->nr_sectors * UBLK_SECTOR_SIZE;
	void	*buf	 = (char *)q->dma_pool +
			   (size_t)tag * q->max_io_buf_bytes;
	struct unvme_cmd *cmd;
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	int ret = 0;

	if (op == UBLK_IO_OP_FLUSH) {
		cmd = unvmed_alloc_cmd_nodata(q->u, q->usq, NULL);
		if (!cmd)
			return -EBUSY;

		union nvme_cmd sqe = {};
		sqe.opcode = nvme_cmd_flush;
		sqe.nsid   = cpu_to_le32(q->nsid);

		if (unvmed_cmd_prep(cmd, &sqe, NULL, 0) < 0) {
			unvmed_cmd_put(cmd);
			return -errno;
		}
	} else if (op == UBLK_IO_OP_WRITE) {
		/* fetch write data from kernel before NVMe submit */
		if (pread(q->cdev_fd, buf, len,
			  (off_t)tag * q->max_io_buf_bytes) < 0)
			return -errno;

		cmd = unvmed_alloc_cmd(q->u, q->usq, NULL, buf, len);
		if (!cmd)
			return -EBUSY;

		ret = unvmed_cmd_prep_write(cmd, q->nsid, slba,
					    (uint16_t)(nlb_cnt - 1),
					    0, 0, 0, 0, 0, false,
					    &iov, 1, NULL, NULL);
		if (ret < 0) {
			unvmed_cmd_put(cmd);
			return -errno;
		}
	} else {	/* READ */
		cmd = unvmed_alloc_cmd(q->u, q->usq, NULL, buf, len);
		if (!cmd)
			return -EBUSY;

		ret = unvmed_cmd_prep_read(cmd, q->nsid, slba,
					   (uint16_t)(nlb_cnt - 1),
					   0, 0, 0, 0, 0, false,
					   &iov, 1, NULL, NULL);
		if (ret < 0) {
			unvmed_cmd_put(cmd);
			return -errno;
		}
	}

	/*
	 * Record the mapping before posting so poller_thread can find the
	 * cmd from the NVMe CQE's CID field.
	 */
	q->cid_to_tag[cmd->cid] = tag;
	__atomic_store_n(&q->pending_cmds[tag], cmd, __ATOMIC_RELEASE);

	unvmed_sq_enter(q->usq);
	unvmed_cmd_post(cmd, &cmd->sqe, 0);
	unvmed_sq_exit(q->usq);

	return 0;
}

/* -------------------------------------------------------------------------
 * NVMe poller thread
 *
 * Runs a tight poll loop on the NVMe CQ.  For each completed command:
 *   1. If READ and successful, pwrite() the DMA buffer back to the
 *      kernel's ublk buffer via the character device.
 *   2. Push (tag, result) onto the SPSC completion ring.
 *   3. Signal the queue_thread via eventfd.
 *   4. Release the cmd (dec refcnt → free + IOMMU unmap).
 * ---------------------------------------------------------------------- */

static void *poller_thread_fn(void *arg)
{
	struct ublk_queue *q = arg;
	struct nvme_cqe cqes[UBLK_MAX_QUEUE_DEPTH];
	const uint64_t one = 1;

	while (q->running) {
		unvmed_cq_enter(q->ucq);
		int n = unvmed_cq_run(q->u, q->usq, q->ucq, cqes);
		unvmed_cq_exit(q->ucq);

		for (int i = 0; i < n; i++) {
			uint16_t cid = cqes[i].cid;
			uint16_t tag = q->cid_to_tag[cid];
			int32_t  res = unvmed_cqe_status(&cqes[i]) ? -EIO : 0;

			struct unvme_cmd *cmd =
				__atomic_load_n(&q->pending_cmds[tag],
						__ATOMIC_ACQUIRE);

			/* for READ: copy result data back to kernel buffer */
			if (res == 0 &&
			    ublksrv_get_op(&q->server->io_descs[q->qid][tag])
					== UBLK_IO_OP_READ) {
				const struct ublksrv_io_desc *iod =
					&q->server->io_descs[q->qid][tag];
				size_t len = (size_t)iod->nr_sectors *
					     UBLK_SECTOR_SIZE;
				void *buf = (char *)q->dma_pool +
					    (size_t)tag * q->max_io_buf_bytes;

				if (pwrite(q->cdev_fd, buf, len,
					   (off_t)tag * q->max_io_buf_bytes) < 0)
					res = -errno;
			}

			/* push to SPSC ring (single producer: this thread) */
			uint32_t tail = atomic_load_explicit(
				&q->comp_tail, memory_order_relaxed);
			q->comp_ring[tail & COMP_RING_MASK] =
				(struct comp_entry){ .tag = tag, .result = res };
			atomic_store_explicit(&q->comp_tail, tail + 1,
					      memory_order_release);

			/* wake queue_thread */
			ssize_t __w = write(q->efd, &one, sizeof(one));
			(void)__w;

			unvmed_cmd_put(cmd);
		}

		if (!n)
			sched_yield();
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * Queue thread  (io_uring event loop)
 *
 * Pre-populates one FETCH_REQ SQE per tag so the kernel knows to deliver
 * I/O requests to us.  Then loops:
 *   - FETCH_REQ CQE → new I/O request → submit_nvme_io()
 *   - eventfd CQE  → drain SPSC ring → COMMIT_AND_FETCH_REQ per entry
 * ---------------------------------------------------------------------- */

static void *queue_thread_fn(void *arg)
{
	struct ublk_queue *q = arg;
	struct io_uring *ring = &q->ring;
	struct io_uring_cqe *cqe;
	int ret;

	/* seed the kernel with one FETCH_REQ per tag */
	for (uint32_t t = 0; t < q->depth; t++) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
		prep_fetch_req(sqe, q->cdev_fd, (uint16_t)q->qid,
			       (uint16_t)t);
	}
	/* arm the eventfd poller */
	{
		struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
		prep_poll_efd(sqe, q->efd);
	}
	io_uring_submit(ring);

	/*
	 * Signal the main thread that this queue's FETCH_REQs are in flight.
	 * START_DEV blocks in the kernel until every queue has at least one
	 * pending FETCH_REQ, so the main thread must not call START_DEV before
	 * all queue threads reach this point.
	 */
	atomic_fetch_add_explicit(&q->server->queues_ready, 1,
				  memory_order_release);

	while (q->running) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			break;
		}

		uint64_t ud  = cqe->user_data;
		int	 res = cqe->res;
		io_uring_cqe_seen(ring, cqe);

		if (ud == UD_EFD_COOKIE) {
			/*
			 * eventfd fired: one or more NVMe completions are
			 * waiting in the SPSC ring.
			 */
			uint64_t val;
			ssize_t __r = read(q->efd, &val, sizeof(val));
			(void)__r;

			uint32_t head = atomic_load_explicit(
				&q->comp_head, memory_order_relaxed);
			uint32_t tail = atomic_load_explicit(
				&q->comp_tail, memory_order_acquire);

			while (head != tail) {
				struct comp_entry ce =
					q->comp_ring[head & COMP_RING_MASK];
				head++;

				struct io_uring_sqe *sqe =
					io_uring_get_sqe(ring);
				prep_commit_and_fetch(sqe, q->cdev_fd,
						      (uint16_t)q->qid,
						      ce.tag, ce.result);
				io_uring_submit(ring);
			}
			atomic_store_explicit(&q->comp_head, head,
					      memory_order_relaxed);

			/* re-arm POLL_ADD so the next eventfd write wakes us */
			{
				struct io_uring_sqe *sqe =
					io_uring_get_sqe(ring);
				prep_poll_efd(sqe, q->efd);
				io_uring_submit(ring);
			}

		} else {
			/*
			 * FETCH_REQ or COMMIT_AND_FETCH CQE: a new I/O request
			 * is available (or a previous commit completed and a new
			 * request is fetched simultaneously).
			 */
			if (res == UBLK_IO_RES_ABORT || !q->running)
				break;

			uint16_t tag = (uint16_t)(ud & 0xFFFFu);
			if (submit_nvme_io(q, tag)) {
				/*
				 * If NVMe submit fails immediately (e.g. the
				 * queue is full), complete with EIO so the block
				 * layer does not stall indefinitely.
				 */
				struct io_uring_sqe *sqe =
					io_uring_get_sqe(ring);
				prep_commit_and_fetch(sqe, q->cdev_fd,
						      (uint16_t)q->qid,
						      tag, -EIO);
				io_uring_submit(ring);
			}
		}
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * io_desc shared-memory mapping
 *
 * The kernel places ublksrv_io_desc entries for queue Q at:
 *   /dev/ublkc<N> file offset:
 *       UBLKSRV_CMD_BUF_OFFSET + Q * UBLK_MAX_QUEUE_DEPTH * sizeof(io_desc)
 * We mmap exactly queue_depth entries for this queue.
 * ---------------------------------------------------------------------- */

static struct ublksrv_io_desc *map_io_descs(int cdev_fd, int qid,
					    uint32_t depth)
{
	size_t stride = (size_t)UBLK_MAX_QUEUE_DEPTH *
			sizeof(struct ublksrv_io_desc);
	off_t  off    = UBLKSRV_CMD_BUF_OFFSET + (off_t)qid * stride;
	size_t sz     = (size_t)depth * sizeof(struct ublksrv_io_desc);

	/*
	 * The io_desc area is written by the kernel; userspace only reads it.
	 * Requesting PROT_WRITE causes the kernel's ublk_ch_mmap handler to
	 * return -EPERM (it checks VM_WRITE and rejects it).
	 */
	void *p = mmap(NULL, sz, PROT_READ,
		       MAP_SHARED | MAP_POPULATE, cdev_fd, off);
	return (p == MAP_FAILED) ? NULL : (struct ublksrv_io_desc *)p;
}

/* -------------------------------------------------------------------------
 * Per-queue resource management
 * ---------------------------------------------------------------------- */

static int queue_init(struct ublk_queue *q, struct unvme_ublk_server *srv,
		      int qid, uint8_t lba_shift, uint32_t max_io_buf_bytes)
{
	char cdev_path[64];
	int  ret;

	q->server		= srv;
	q->dev_id		= srv->dev_id;
	q->qid			= qid;
	q->depth		= srv->queue_depth;
	q->u			= srv->u;
	q->nsid			= srv->nsid;
	q->lba_shift		= lba_shift;
	q->max_io_buf_bytes	= max_io_buf_bytes;
	q->running		= true;
	atomic_init(&q->comp_head, 0);
	atomic_init(&q->comp_tail, 0);

	/* bind to the NVMe SQ/CQ pair assigned to this queue */
	int sqid = srv->start_sqid + qid;
	q->usq = unvmed_sq_get(srv->u, sqid);
	if (!q->usq) {
		fprintf(stderr, "ublk: SQ %d not found\n", sqid);
		return -ENOENT;
	}
	q->ucq = q->usq->ucq;

	q->efd = eventfd(0, EFD_NONBLOCK);
	if (q->efd < 0)
		return -errno;

	snprintf(cdev_path, sizeof(cdev_path), UBLK_CDEV_FMT, srv->dev_id);
	q->cdev_fd = open(cdev_path, O_RDWR);
	if (q->cdev_fd < 0) {
		ret = -errno;
		goto err_efd;
	}

	/*
	 * IORING_SETUP_SQE128: ublk I/O commands (FETCH_REQ, COMMIT_AND_FETCH)
	 * embed struct ublksrv_io_cmd (16 bytes) in sqe->cmd[], which lives at
	 * offset 64 of the SQE.  Without 128-byte SQEs there is no space there,
	 * and writing to sqe->cmd corrupts the adjacent SQE slot.
	 */
	{
		struct io_uring_params p = { .flags = IORING_SETUP_SQE128 };
		ret = io_uring_queue_init_params(q->depth * 2 + 4, &q->ring, &p);
	}
	if (ret) {
		ret = -ret;
		goto err_cdev;
	}

	srv->io_descs[qid] = map_io_descs(q->cdev_fd, qid, q->depth);
	if (!srv->io_descs[qid]) {
		ret = -errno;
		goto err_ring;
	}

	/* pre-allocated DMA buffer pool: page-aligned, IOMMU-mapped */
	size_t pool_size = (size_t)q->depth * max_io_buf_bytes;
	q->dma_pool = aligned_alloc(4096, pool_size);
	if (!q->dma_pool) {
		ret = -ENOMEM;
		goto err_descs;
	}
	if (unvmed_map_vaddr(q->u, q->dma_pool, pool_size, NULL, 0)) {
		ret = -errno;
		goto err_pool;
	}

	q->pending_cmds = calloc(q->depth, sizeof(*q->pending_cmds));
	q->cid_to_tag   = calloc(q->depth, sizeof(*q->cid_to_tag));
	if (!q->pending_cmds || !q->cid_to_tag) {
		ret = -ENOMEM;
		goto err_iova;
	}

	return 0;

err_iova:
	free(q->cid_to_tag);
	free(q->pending_cmds);
	unvmed_unmap_vaddr(q->u, q->dma_pool);
err_pool:
	free(q->dma_pool);
err_descs:
	munmap(srv->io_descs[qid],
	       (size_t)q->depth * sizeof(struct ublksrv_io_desc));
	srv->io_descs[qid] = NULL;
err_ring:
	io_uring_queue_exit(&q->ring);
err_cdev:
	close(q->cdev_fd);
err_efd:
	close(q->efd);
	return ret;
}

static void queue_teardown(struct ublk_queue *q, struct unvme_ublk_server *srv,
			   int qid)
{
	free(q->cid_to_tag);
	free(q->pending_cmds);

	if (q->dma_pool) {
		unvmed_unmap_vaddr(q->u, q->dma_pool);
		free(q->dma_pool);
	}

	if (srv->io_descs[qid]) {
		munmap(srv->io_descs[qid],
		       (size_t)q->depth * sizeof(struct ublksrv_io_desc));
		srv->io_descs[qid] = NULL;
	}

	io_uring_queue_exit(&q->ring);
	close(q->cdev_fd);
	close(q->efd);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int unvme_ublk_server_start(struct unvme *u, uint32_t nsid,
			    uint64_t nr_sectors, uint8_t lba_shift,
			    int dev_id, int nr_queues, uint32_t queue_depth,
			    int start_sqid, uint32_t max_io_kb)
{
	struct unvme_ublk_server *srv;
	uint32_t max_io_buf_bytes = max_io_kb * 1024u;
	uint32_t max_sectors      = max_io_buf_bytes >> UBLK_SECTOR_SHIFT;
	int ret, i;

	if (dev_id < 0 || dev_id >= MAX_UBLK_SERVERS || nr_queues <= 0 ||
	    !queue_depth || !max_io_kb)
		return -EINVAL;

	pthread_mutex_lock(&g_servers_mutex);
	if (g_servers[dev_id]) {
		pthread_mutex_unlock(&g_servers_mutex);
		return -EEXIST;
	}

	srv = calloc(1, sizeof(*srv));
	if (!srv) {
		pthread_mutex_unlock(&g_servers_mutex);
		return -ENOMEM;
	}

	srv->dev_id      = dev_id;
	srv->nr_queues   = nr_queues;
	srv->queue_depth = queue_depth;
	srv->u           = u;
	srv->nsid        = nsid;
	srv->start_sqid  = start_sqid;
	srv->running     = true;

	srv->queues         = calloc(nr_queues, sizeof(*srv->queues));
	srv->queue_threads  = calloc(nr_queues, sizeof(pthread_t));
	srv->poller_threads = calloc(nr_queues, sizeof(pthread_t));
	srv->io_descs       = calloc(nr_queues, sizeof(*srv->io_descs));
	if (!srv->queues || !srv->queue_threads ||
	    !srv->poller_threads || !srv->io_descs) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	srv->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (srv->ctrl_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ublk: open %s: %s\n",
			UBLK_CTRL_DEV, strerror(errno));
		goto err_alloc;
	}

	/* Clean up any leftover device from a previous failed attempt. */
	ublk_stop_dev(srv->ctrl_fd, dev_id);
	ublk_del_dev(srv->ctrl_fd, dev_id);

	ret = ublk_add_dev(srv->ctrl_fd, dev_id, nr_queues,
			   queue_depth, max_io_buf_bytes);
	if (ret) {
		fprintf(stderr, "ublk: ADD_DEV (dev=%d): %s\n",
			dev_id, strerror(-ret));
		goto err_ctrl;
	}

	ret = ublk_set_params(srv->ctrl_fd, dev_id, nr_sectors, lba_shift,
			      max_sectors);
	if (ret) {
		fprintf(stderr, "ublk: SET_PARAMS (dev=%d): %s\n",
			dev_id, strerror(-ret));
		goto err_del;
	}

	for (i = 0; i < nr_queues; i++) {
		ret = queue_init(&srv->queues[i], srv, i,
				 lba_shift, max_io_buf_bytes);
		if (ret) {
			fprintf(stderr, "ublk: queue %d init: %s\n",
				i, strerror(-ret));
			while (--i >= 0)
				queue_teardown(&srv->queues[i], srv, i);
			goto err_del;
		}
	}

	/*
	 * Queue threads must submit their FETCH_REQs before we call START_DEV.
	 * The kernel blocks START_DEV until every queue has a pending FETCH_REQ
	 * in its io_uring ring.  Start threads first, then wait for all of them
	 * to increment queues_ready, then call START_DEV.
	 */
	atomic_init(&srv->queues_ready, 0);

	for (i = 0; i < nr_queues; i++) {
		pthread_create(&srv->queue_threads[i],  NULL,
			       queue_thread_fn,  &srv->queues[i]);
		pthread_create(&srv->poller_threads[i], NULL,
			       poller_thread_fn, &srv->queues[i]);
	}

	while (atomic_load_explicit(&srv->queues_ready, memory_order_acquire)
	       < nr_queues)
		sched_yield();

	ret = ublk_start_dev(srv->ctrl_fd, dev_id);
	if (ret) {
		fprintf(stderr, "ublk: START_DEV (dev=%d): %s\n",
			dev_id, strerror(-ret));
		srv->running = false;
		for (i = 0; i < nr_queues; i++)
			srv->queues[i].running = false;
		/*
		 * Inject UBLK_IO_RES_ABORT into all pending FETCH_REQ
		 * io_uring_cmds so queue_threads unblock from io_uring_wait_cqe.
		 * STOP_DEV may fail if device is still in DEAD state; DEL_DEV
		 * calls ublk_mark_io_dead() unconditionally, which posts the
		 * ABORT completion to every pending FETCH before returning.
		 */
		ublk_stop_dev(srv->ctrl_fd, dev_id);
		ublk_del_dev(srv->ctrl_fd, dev_id);
		for (i = 0; i < nr_queues; i++) {
			pthread_join(srv->queue_threads[i],  NULL);
			pthread_join(srv->poller_threads[i], NULL);
		}
		for (i = 0; i < nr_queues; i++)
			queue_teardown(&srv->queues[i], srv, i);
		goto err_ctrl;	/* device already deleted above */
	}

	g_servers[dev_id] = srv;
	pthread_mutex_unlock(&g_servers_mutex);

	fprintf(stdout,
		"ublk: " UBLK_BDEV_FMT " started "
		"(nsid=%u queues=%d depth=%u lba=%uB max_io=%uKiB)\n",
		dev_id, nsid, nr_queues, queue_depth,
		1u << lba_shift, max_io_kb);
	return 0;

err_del:
	ublk_del_dev(srv->ctrl_fd, dev_id);
err_ctrl:
	close(srv->ctrl_fd);
err_alloc:
	free(srv->io_descs);
	free(srv->poller_threads);
	free(srv->queue_threads);
	free(srv->queues);
	free(srv);
	pthread_mutex_unlock(&g_servers_mutex);
	return ret;
}

int unvme_ublk_server_stop(int dev_id)
{
	struct unvme_ublk_server *srv;
	int i;

	if (dev_id < 0 || dev_id >= MAX_UBLK_SERVERS)
		return -EINVAL;

	pthread_mutex_lock(&g_servers_mutex);
	srv = g_servers[dev_id];
	if (!srv) {
		pthread_mutex_unlock(&g_servers_mutex);
		return -ENOENT;
	}
	g_servers[dev_id] = NULL;
	pthread_mutex_unlock(&g_servers_mutex);

	/* signal threads to stop */
	srv->running = false;
	for (i = 0; i < srv->nr_queues; i++)
		srv->queues[i].running = false;

	/*
	 * STOP_DEV causes the kernel to inject UBLK_IO_RES_ABORT into all
	 * pending FETCH_REQ CQEs, which unblocks the queue_threads.
	 */
	ublk_stop_dev(srv->ctrl_fd, dev_id);

	for (i = 0; i < srv->nr_queues; i++) {
		pthread_join(srv->queue_threads[i],  NULL);
		pthread_join(srv->poller_threads[i], NULL);
	}

	for (i = 0; i < srv->nr_queues; i++)
		queue_teardown(&srv->queues[i], srv, i);

	ublk_del_dev(srv->ctrl_fd, dev_id);
	close(srv->ctrl_fd);

	free(srv->io_descs);
	free(srv->poller_threads);
	free(srv->queue_threads);
	free(srv->queues);
	free(srv);

	fprintf(stdout, "ublk: " UBLK_BDEV_FMT " stopped\n", dev_id);
	return 0;
}
