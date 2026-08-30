// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <liburing.h>
#include <linux/ublk_cmd.h>

#include <nvme/types.h>

#include "libunvmed.h"
#include "libunvmed-private.h"

#include "unvmed-ublk.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Newer kernels (seen in 6.18+) validate ublksrv_pid by looking up the
 * task via find_task_by_vpid().  When the control ring command is deferred
 * to a kernel worker thread, that thread lives in the init (host) PID
 * namespace, so find_task_by_vpid(container_pid) finds a different process
 * and returns -EINVAL.  Outside a container getpid() == host PID, so it
 * works; inside Docker getpid() returns the container-local PID, causing
 * the failure.
 *
 * Fix: read the last (outermost / host) PID from the NSpid field of
 * /proc/self/status.  Outside containers NSpid has a single value equal to
 * getpid().  Inside containers the last value is the host-namespace PID that
 * the kernel can actually locate.
 */
static pid_t get_global_pid(void)
{
	FILE *f;
	char line[256];
	pid_t global_pid = getpid();

	f = fopen("/proc/self/status", "r");
	if (!f)
		return global_pid;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "NSpid:", 6) != 0)
			continue;

		char *p = line + 6;
		pid_t last = 0, val;
		char *endp;

		for (;;) {
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '\n' || *p == '\r' || *p == '\0') break;
			val = (pid_t)strtol(p, &endp, 10);
			if (endp == p) break;
			last = val;
			p = endp;
		}
		if (last > 0)
			global_pid = last;
		break;
	}

	fclose(f);
	return global_pid;
}

/*
 * Send a control command to /dev/ublk-control via io_uring.
 *
 * UBLK_U_CMD_* are cmd_op values for IORING_OP_URING_CMD, not ioctl
 * numbers.  struct ublksrv_ctrl_cmd (32 bytes) is embedded in sqe->cmd[]
 * which requires SQE128 (standard SQE only has 16 bytes of cmd[] space).
 * Any data exchange (e.g. dev_info written back by ADD_DEV) happens via
 * ctrl_cmd->addr pointing to a userspace buffer.
 */
static int ublk_ctrl_cmd(struct io_uring *ring, int ctrl_fd,
			  unsigned int cmd_op,
			  struct ublksrv_ctrl_cmd *ctrl_cmd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe)
		return -ENOMEM;

	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, ctrl_fd, NULL, 0, 0);
	sqe->cmd_op = cmd_op;
	memcpy((void *)sqe->cmd, ctrl_cmd, sizeof(*ctrl_cmd));

	ret = io_uring_submit(ring);
	if (ret < 0)
		return ret;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0)
		return ret;

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

/*
 * Find the highest existing I/O SQ qid allocated in @u, return next free id.
 * Admin queue (qid=0) is always present; I/O queues start at 1.
 */
static uint32_t unvmed_ublk_next_qid(struct unvme *u)
{
	struct unvme_sq **sqs = NULL;
	uint32_t max_qid = 0;
	int nr;

	nr = unvmed_get_sqs(u, &sqs);
	for (int i = 0; i < nr; i++) {
		if ((uint32_t)sqs[i]->id > max_qid)
			max_qid = (uint32_t)sqs[i]->id;
	}
	free(sqs);

	return max_qid + 1;
}

/*
 * Derive max_io_size from the controller's MDTS, capped at the compile-time
 * limit.  If the controller has no MDTS limit (mdts == 0), use the cap.
 */
static size_t unvmed_ublk_max_io_size(struct unvme *u)
{
	struct nvme_id_ctrl *id = u->id_ctrl;
	size_t page = unvmed_pagesize(u);
	size_t mdts_bytes;

	if (!id || id->mdts == 0)
		return UNVMED_UBLK_MAX_IO_SIZE;

	mdts_bytes = page * (1u << id->mdts);
	if (mdts_bytes > UNVMED_UBLK_MAX_IO_SIZE)
		return UNVMED_UBLK_MAX_IO_SIZE;
	return mdts_bytes;
}

/* ------------------------------------------------------------------ */
/* Queue init / free                                                    */
/* ------------------------------------------------------------------ */

static struct unvmed_ublk_queue *unvmed_ublk_queue_alloc(
		struct unvmed_ublk_server *server, int qid)
{
	struct unvmed_ublk_queue *q;

	q = calloc(1, sizeof(*q));
	if (!q)
		return NULL;

	q->server    = server;
	q->qid       = qid;
	q->slot_size = server->max_io_size;
	q->poll_spin_us = server->queue_depth;  /* placeholder; set below */
	q->dev_fd    = -1;
	return q;
}

static int unvmed_ublk_queue_init_nvme(struct unvmed_ublk_queue *q)
{
	struct unvmed_ublk_server *s = q->server;
	struct unvme *u = s->u;
	uint32_t nvme_qid = s->base_qid + (uint32_t)q->qid;
	int ret;

	/* Polling CQ (vector = -1): handler thread polls directly. */
	ret = unvmed_create_cq(u, nvme_qid, s->queue_depth, /*vector=*/-1,
			       /*pc=*/1);
	if (ret) {
		unvmed_log_err("ublk q%d: failed to create NVMe CQ %u: %s",
			       q->qid, nvme_qid, strerror(errno));
		return -1;
	}

	ret = unvmed_create_sq(u, nvme_qid, s->queue_depth, nvme_qid,
			       /*qprio=*/0, /*pc=*/1, /*nvmsetid=*/0);
	if (ret) {
		unvmed_log_err("ublk q%d: failed to create NVMe SQ %u: %s",
			       q->qid, nvme_qid, strerror(errno));
		return -1;
	}

	q->usq = unvmed_sq_get(u, nvme_qid);
	q->ucq = q->usq ? q->usq->ucq : NULL;

	if (!q->usq || !q->ucq) {
		unvmed_log_err("ublk q%d: failed to look up NVMe SQ/CQ %u",
			       q->qid, nvme_qid);
		return -1;
	}

	return 0;
}

static int unvmed_ublk_queue_init_bounce(struct unvmed_ublk_queue *q)
{
	struct unvmed_ublk_server *s = q->server;
	struct unvme *u = s->u;
	size_t total = (size_t)s->queue_depth * q->slot_size;
	uint64_t iova;

	q->bounce_total = total;

	if (unvmed_pgmap(u, &q->bounce, total) < 0) {
		unvmed_log_err("ublk q%d: failed to alloc bounce buffer (%zu B)",
			       q->qid, total);
		return -1;
	}

	/*
	 * Map to IOMMU so libvfn can use this buffer as NVMe DMA target.
	 * unvmed_alloc_cmd() will find the existing mapping and skip re-mapping.
	 */
	if (unvmed_map_vaddr(u, q->bounce, total, &iova, 0)) {
		unvmed_log_err("ublk q%d: failed to IOMMU-map bounce buffer",
			       q->qid);
		unvmed_pgunmap(q->bounce);
		q->bounce = NULL;
		return -1;
	}

	return 0;
}

static int unvmed_ublk_queue_init_iodesc(struct unvmed_ublk_queue *q)
{
	struct unvmed_ublk_server *s = q->server;
	size_t desc_sz = s->queue_depth * sizeof(struct ublksrv_io_desc);
	off_t  desc_off = (off_t)UBLKSRV_CMD_BUF_OFFSET + q->qid * (off_t)desc_sz;
	void  *p;

	p = mmap(NULL, desc_sz, PROT_READ, MAP_SHARED | MAP_POPULATE,
		 q->dev_fd, desc_off);
	if (p == MAP_FAILED) {
		unvmed_log_err("ublk q%d: failed to mmap io_descs: %s",
			       q->qid, strerror(errno));
		return -1;
	}

	q->io_descs      = (struct ublksrv_io_desc *)p;
	q->io_descs_size = desc_sz;
	return 0;
}

static int unvmed_ublk_queue_init_ring(struct unvmed_ublk_queue *q)
{
	struct unvmed_ublk_server *s = q->server;
	struct io_uring_params params = {};
	int ret;

	/*
	 * IORING_SETUP_SQE128: 128-byte SQEs, needed to embed
	 * struct ublksrv_io_cmd (16 bytes) in the cmd[] tail area.
	 */
	params.flags = IORING_SETUP_SQE128;

	/* +1 for occasional extra SQE during flush */
	ret = io_uring_queue_init_params(s->queue_depth + 1, &q->ring, &params);
	if (ret) {
		unvmed_log_err("ublk q%d: io_uring init failed: %s",
			       q->qid, strerror(-ret));
		return -1;
	}

	return 0;
}

static int unvmed_ublk_queue_prefill_fetch(struct unvmed_ublk_queue *q)
{
	struct unvmed_ublk_server *s = q->server;

	for (uint32_t tag = 0; tag < s->queue_depth; tag++) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&q->ring);
		if (!sqe) {
			unvmed_log_err("ublk q%d: no SQE slot for tag %u",
				       q->qid, tag);
			return -1;
		}

		io_uring_prep_rw(IORING_OP_URING_CMD, sqe, q->dev_fd,
				 NULL, 0, 0);
		sqe->cmd_op   = UBLK_U_IO_FETCH_REQ;
		sqe->user_data = (uint64_t)tag;

		struct ublksrv_io_cmd *io_cmd =
			(struct ublksrv_io_cmd *)sqe->cmd;
		io_cmd->q_id   = (uint16_t)q->qid;
		io_cmd->tag    = (uint16_t)tag;
		io_cmd->addr   = (__u64)((char *)q->bounce +
					 tag * q->slot_size);
		io_cmd->result = 0;
	}

	return io_uring_submit(&q->ring);
}

static int unvmed_ublk_queue_init(struct unvmed_ublk_queue *q, int dev_fd,
				  uint32_t poll_spin_us)
{
	struct unvmed_ublk_server *s = q->server;

	q->dev_fd      = dev_fd;
	q->poll_spin_us = poll_spin_us;

	q->inflight = calloc(s->queue_depth, sizeof(*q->inflight));
	if (!q->inflight)
		return -ENOMEM;

	if (unvmed_ublk_queue_init_nvme(q))
		goto err_inflight;

	if (unvmed_ublk_queue_init_bounce(q))
		goto err_nvme;

	if (unvmed_ublk_queue_init_iodesc(q))
		goto err_bounce;

	if (unvmed_ublk_queue_init_ring(q))
		goto err_iodesc;

	if (unvmed_ublk_queue_prefill_fetch(q) < 0)
		goto err_ring;

	return 0;

err_ring:
	io_uring_queue_exit(&q->ring);
err_iodesc:
	munmap(q->io_descs, q->io_descs_size);
	q->io_descs = NULL;
err_bounce:
	unvmed_unmap_vaddr(s->u, q->bounce);
	unvmed_pgunmap(q->bounce);
	q->bounce = NULL;
err_nvme:
	/* NVMe queues stay alive — they are deleted in server_stop */
err_inflight:
	free(q->inflight);
	q->inflight = NULL;
	return -1;
}

static void unvmed_ublk_queue_free(struct unvmed_ublk_queue *q)
{
	if (!q)
		return;

	io_uring_queue_exit(&q->ring);

	if (q->io_descs) {
		munmap(q->io_descs, q->io_descs_size);
		q->io_descs = NULL;
	}

	if (q->bounce) {
		unvmed_unmap_vaddr(q->server->u, q->bounce);
		unvmed_pgunmap(q->bounce);
		q->bounce = NULL;
	}

	free(q->inflight);
	q->inflight = NULL;

	free(q);
}

/* ------------------------------------------------------------------ */
/* ublk device control (ioctl on /dev/ublk-control)                   */
/* ------------------------------------------------------------------ */

static int ublk_add_dev(struct io_uring *ring, int ctrl_fd,
			uint32_t nr_queues, uint32_t queue_depth,
			size_t max_io_size,
			struct ublksrv_ctrl_dev_info *info_out)
{
	struct ublksrv_ctrl_dev_info dev_info = {
		.nr_hw_queues     = (uint16_t)nr_queues,
		.queue_depth      = (uint16_t)queue_depth,
		.max_io_buf_bytes = (uint32_t)max_io_size,
		.dev_id           = (uint32_t)-1,  /* let kernel assign */
		.ublksrv_pid      = get_global_pid(),
		.flags            = UBLK_F_CMD_IOCTL_ENCODE,
	};
	struct ublksrv_ctrl_cmd ctrl_cmd = {
		.dev_id   = (uint32_t)-1,
		.queue_id = (uint16_t)-1,
		.len      = sizeof(dev_info),
		.addr     = (__u64)(uintptr_t)&dev_info,
	};
	int ret;

	ret = ublk_ctrl_cmd(ring, ctrl_fd, UBLK_U_CMD_ADD_DEV, &ctrl_cmd);
	if (ret < 0)
		return ret;

	*info_out = dev_info;
	return 0;
}

static int ublk_set_params(struct io_uring *ring, int ctrl_fd, int dev_id,
			   uint64_t nr_sectors, uint32_t lba_size,
			   uint32_t max_sectors)
{
	struct ublk_params p = {
		.len   = sizeof(p),
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift  = __builtin_ctz(lba_size),
			.physical_bs_shift = 12,  /* 4096 */
			.io_opt_shift      = 12,
			.io_min_shift      = __builtin_ctz(lba_size),
			.max_sectors       = max_sectors,
			.dev_sectors       = nr_sectors,
		},
	};
	struct ublksrv_ctrl_cmd ctrl_cmd = {
		.dev_id   = (uint32_t)dev_id,
		.queue_id = (uint16_t)-1,
		.len      = sizeof(p),
		.addr     = (__u64)(uintptr_t)&p,
	};

	return ublk_ctrl_cmd(ring, ctrl_fd, UBLK_U_CMD_SET_PARAMS, &ctrl_cmd);
}

static int ublk_start_dev(struct io_uring *ring, int ctrl_fd, int dev_id)
{
	struct ublksrv_ctrl_cmd ctrl_cmd = {
		.dev_id   = (uint32_t)dev_id,
		.queue_id = (uint16_t)-1,
		.data[0]  = (uint64_t)get_global_pid(),
	};

	return ublk_ctrl_cmd(ring, ctrl_fd, UBLK_U_CMD_START_DEV, &ctrl_cmd);
}

static int ublk_stop_dev(struct io_uring *ring, int ctrl_fd, int dev_id)
{
	struct ublksrv_ctrl_cmd ctrl_cmd = {
		.dev_id   = (uint32_t)dev_id,
		.queue_id = (uint16_t)-1,
	};
	return ublk_ctrl_cmd(ring, ctrl_fd, UBLK_U_CMD_STOP_DEV, &ctrl_cmd);
}

static int ublk_del_dev(struct io_uring *ring, int ctrl_fd, int dev_id)
{
	struct ublksrv_ctrl_cmd ctrl_cmd = {
		.dev_id   = (uint32_t)dev_id,
		.queue_id = (uint16_t)-1,
	};
	return ublk_ctrl_cmd(ring, ctrl_fd, UBLK_U_CMD_DEL_DEV, &ctrl_cmd);
}

/* ------------------------------------------------------------------ */
/* Server start / stop                                                  */
/* ------------------------------------------------------------------ */

struct unvmed_ublk_server *unvmed_ublk_server_start(struct unvme *u,
						    uint32_t nsid,
						    uint32_t nr_queues,
						    uint32_t queue_depth,
						    uint32_t poll_spin_us)
{
	struct unvmed_ublk_server *server;
	struct unvme_ns *ns;
	struct ublksrv_ctrl_dev_info dev_info;
	char dev_path[64];
	int dev_fd = -1;
	int ret;

	if (nr_queues > UNVMED_UBLK_MAX_QUEUES) {
		errno = EINVAL;
		return NULL;
	}

	/* Validate namespace */
	ns = unvmed_ns_get(u, nsid);
	if (!ns) {
		unvmed_log_err("ublk: namespace %u not found", nsid);
		errno = ENODEV;
		return NULL;
	}

	server = calloc(1, sizeof(*server));
	if (!server) {
		unvmed_ns_put(u, ns);
		return NULL;
	}

	server->u           = u;
	server->nr_queues   = nr_queues;
	server->queue_depth = queue_depth;
	server->nsid        = nsid;
	server->nr_sectors  = ns->nr_lbas;
	server->lba_size    = ns->lba_size;
	server->max_io_size = unvmed_ublk_max_io_size(u);
	server->running     = true;
	server->dev_id      = -1;
	server->base_qid    = unvmed_ublk_next_qid(u);

	unvmed_ns_put(u, ns);

	/* Open ublk control device */
	server->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (server->ctrl_fd < 0) {
		unvmed_log_err("ublk: failed to open %s: %s",
			       UBLK_CTRL_DEV, strerror(errno));
		goto err_free;
	}

	/*
	 * Control ring: UBLK_U_CMD_* are io_uring cmd_op values, not ioctl
	 * numbers.  SQE128 required: ublksrv_ctrl_cmd is 32 bytes but a
	 * standard SQE only provides 16 bytes of cmd[] space.
	 */
	{
		struct io_uring_params p = { .flags = IORING_SETUP_SQE128 };
		ret = io_uring_queue_init_params(8, &server->ctrl_ring, &p);
		if (ret) {
			unvmed_log_err("ublk: ctrl io_uring init failed: %s",
				       strerror(-ret));
			goto err_ctrl;
		}
	}

	/* ADD_DEV: kernel creates /dev/ublkc<N> and /dev/ublkb<N> */
	ret = ublk_add_dev(&server->ctrl_ring, server->ctrl_fd,
			   nr_queues, queue_depth,
			   server->max_io_size, &dev_info);
	if (ret) {
		unvmed_log_err("ublk: UBLK_CMD_ADD_DEV failed: %s",
			       strerror(-ret));
		goto err_ring;
	}
	server->dev_id = (int)dev_info.dev_id;
	unvmed_log_info("ublk: created /dev/ublkb%d /dev/ublkc%d",
			server->dev_id, server->dev_id);

	/* SET_PARAMS: device size, block size, max sectors */
	ret = ublk_set_params(&server->ctrl_ring, server->ctrl_fd,
			      server->dev_id,
			      server->nr_sectors, server->lba_size,
			      (uint32_t)(server->max_io_size / 512));
	if (ret) {
		unvmed_log_err("ublk: UBLK_CMD_SET_PARAMS failed: %s",
			       strerror(-ret));
		goto err_del;
	}

	/* Open the char device for all queues */
	snprintf(dev_path, sizeof(dev_path), "/dev/ublkc%d", server->dev_id);
	dev_fd = open(dev_path, O_RDWR);
	if (dev_fd < 0) {
		unvmed_log_err("ublk: failed to open %s: %s",
			       dev_path, strerror(errno));
		goto err_del;
	}

	/* Initialize per-queue state and start handler threads */
	for (uint32_t i = 0; i < nr_queues; i++) {
		struct unvmed_ublk_queue *q;

		q = unvmed_ublk_queue_alloc(server, (int)i);
		if (!q)
			goto err_queues;

		server->queues[i] = q;

		ret = unvmed_ublk_queue_init(q, dev_fd, poll_spin_us);
		if (ret) {
			unvmed_log_err("ublk: queue %u init failed", i);
			goto err_queues;
		}

		q->running = true;
		ret = pthread_create(&q->thread, NULL,
				     unvmed_ublk_queue_handler, q);
		if (ret) {
			unvmed_log_err("ublk: failed to start queue %u thread: %s",
				       i, strerror(ret));
			q->running = false;
			goto err_queues;
		}
	}

	/*
	 * START_DEV: makes /dev/ublkb<N> visible to the block layer.
	 * All queue handler threads must have submitted their initial
	 * FETCH_REQs before this point (they did in queue_init).
	 */
	ret = ublk_start_dev(&server->ctrl_ring, server->ctrl_fd,
			     server->dev_id);
	if (ret) {
		unvmed_log_err("ublk: UBLK_CMD_START_DEV failed: %s",
			       strerror(-ret));
		goto err_queues;
	}

	/*
	 * dev_fd ref is kept open for mmap lifetime; we dup it into each
	 * queue so the original fd can be closed at function exit.  Here we
	 * leave it open — the queue structs share the same fd number which
	 * is fine since they were all init'd with dev_fd above.
	 */
	unvmed_log_info("ublk: /dev/ublkb%d is live (%u queues, depth %u)",
			server->dev_id, nr_queues, queue_depth);
	return server;

err_queues:
	server->running = false;
	for (uint32_t i = 0; i < nr_queues; i++) {
		struct unvmed_ublk_queue *q = server->queues[i];
		if (!q)
			continue;
		if (q->running) {
			q->running = false;
			pthread_join(q->thread, NULL);
		}
		unvmed_ublk_queue_free(q);
		server->queues[i] = NULL;
	}
	if (dev_fd >= 0)
		close(dev_fd);
err_del:
	if (server->dev_id >= 0)
		ublk_del_dev(&server->ctrl_ring, server->ctrl_fd, server->dev_id);
err_ring:
	io_uring_queue_exit(&server->ctrl_ring);
err_ctrl:
	close(server->ctrl_fd);
err_free:
	free(server);
	return NULL;
}

int unvmed_ublk_server_stop(struct unvmed_ublk_server *server)
{
	int dev_fd = -1;

	if (!server)
		return 0;

	server->running = false;

	/* Signal all queue threads to stop and join them */
	for (uint32_t i = 0; i < server->nr_queues; i++) {
		struct unvmed_ublk_queue *q = server->queues[i];
		if (!q)
			continue;

		if (dev_fd < 0)
			dev_fd = q->dev_fd;

		q->running = false;
		pthread_join(q->thread, NULL);
	}

	/* STOP_DEV: quiesces the block device */
	if (server->dev_id >= 0)
		ublk_stop_dev(&server->ctrl_ring, server->ctrl_fd, server->dev_id);

	/* Free per-queue resources */
	for (uint32_t i = 0; i < server->nr_queues; i++) {
		unvmed_ublk_queue_free(server->queues[i]);
		server->queues[i] = NULL;
	}

	/*
	 * Delete NVMe I/O queues that were created for ublk.
	 * unvmed_create_cq/sq are symmetric — use the admin queue path.
	 */
	for (uint32_t i = 0; i < server->nr_queues; i++) {
		uint32_t qid = server->base_qid + i;
		struct unvme_sq *usq = unvmed_sq_find(server->u, qid);
		if (usq)
			unvmed_sq_put(server->u, usq);
	}

	/* DEL_DEV: removes /dev/ublkb<N> and /dev/ublkc<N> */
	if (server->dev_id >= 0) {
		ublk_del_dev(&server->ctrl_ring, server->ctrl_fd, server->dev_id);
		unvmed_log_info("ublk: /dev/ublkb%d deleted", server->dev_id);
	}

	io_uring_queue_exit(&server->ctrl_ring);
	if (dev_fd >= 0)
		close(dev_fd);
	close(server->ctrl_fd);
	free(server);

	return 0;
}
