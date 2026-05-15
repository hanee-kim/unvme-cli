// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
/*
 * libunvmed-ublk.c — ublk server backed by libunvmed
 *
 * OVERVIEW
 * --------
 * This file bridges the Linux ublk kernel interface to NVMe hardware:
 *
 *   Multiple fio processes
 *        ↓  pread/pwrite on /dev/ublkb{N}
 *   kernel block layer  (handles fan-in from multiple processes)
 *        ↓  io_uring  (UBLK_IO_FETCH_REQ / UBLK_IO_COMMIT_AND_FETCH_REQ)
 *   worker threads inside the caller's process   ← this file
 *        ↓  unvmed_alloc_cmd / unvmed_cmd_post / __unvmed_cq_run_n
 *   NVMe hardware via libunvmed + VFIO
 *
 * THREAD MODEL
 * ------------
 * One worker thread per ublk queue.  Each thread owns:
 *   - one io_uring ring (talks to the kernel ublk driver)
 *   - one NVMe SQ/CQ pair (talks to the NVMe device via libunvmed)
 *   - a pool of (queue_depth) slots, each with a pre-allocated NVMe DMA buffer
 *
 * The thread loop:
 *   1. Collect completed io_uring CQEs  → incoming I/O requests from kernel
 *   2. For each request: memcpy write data ublk→DMA, submit NVMe cmd (no doorbell)
 *   3. Ring the NVMe doorbell once for the whole batch
 *   4. Poll NVMe CQ (non-blocking) for completions
 *   5. For each NVMe completion: memcpy read data DMA→ublk, commit ublk request
 *   6. If nothing happened: sleep in io_uring_enter waiting for new ublk requests
 *
 * BUFFER MANAGEMENT
 * -----------------
 * Each slot has two buffers:
 *
 *   ublk_buf  (slot->ublk_buf[tag])
 *     Mmap'd from /dev/ublkc{dev_id}.  The kernel fills this with write data
 *     before delivering a WRITE request, and reads from it after we commit a
 *     READ request.
 *
 *   dma_buf  (slot->dma_buf)
 *     Allocated via unvmed_pgmap() — page-aligned, IOMMU-mapped.  This is
 *     what the NVMe device DMA's into/from.
 *
 *   Write path:  ublk_buf →[memcpy]→ dma_buf →[NVMe DMA]→ disk
 *   Read  path:  disk →[NVMe DMA]→ dma_buf →[memcpy]→ ublk_buf
 */

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
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/io_uring.h>
#include <linux/ublk_cmd.h>
#include <nvme/types.h>

#include "libunvmed.h"
#include "libunvmed-ublk.h"

/* =========================================================================
 * Section 1: Raw io_uring — no liburing dependency
 *
 * We implement just enough of the io_uring interface to drive ublk:
 *   - ring setup (io_uring_setup syscall + mmap)
 *   - SQE submission
 *   - CQE retrieval (blocking and non-blocking)
 *   - ring teardown
 * ========================================================================= */

/* Raw io_uring ring state.  All pointer fields point into mmap'd kernel memory. */
struct ublk_ring {
	int    fd;           /* the io_uring fd returned by io_uring_setup */

	/* SQ ring — mmap at IORING_OFF_SQ_RING */
	void          *sq_mmap;
	size_t         sq_mmap_sz;
	unsigned int  *sq_head;      /* kernel advances this (reads) */
	unsigned int  *sq_tail;      /* we advance this (writes) */
	unsigned int  *sq_mask;      /* ring_entries - 1 */
	unsigned int  *sq_array;     /* index array: sq_array[i] → sqes[i] */
	unsigned int   sq_entries;

	/* SQEs array — mmap at IORING_OFF_SQES, 128 bytes each (SQE128 mode) */
	void          *sqes_mmap;
	size_t         sqes_mmap_sz;
	struct io_uring_sqe *sqes;   /* each entry is really 128 B */

	/* CQ ring — mmap at IORING_OFF_CQ_RING */
	void          *cq_mmap;
	size_t         cq_mmap_sz;
	unsigned int  *cq_head;      /* we advance this (reads) */
	unsigned int  *cq_tail;      /* kernel advances this (writes) */
	unsigned int  *cq_mask;
	struct io_uring_cqe *cqes;
	unsigned int   cq_entries;

	unsigned int   sq_pending;   /* SQEs submitted but not yet entered */
};

static int io_uring_setup_syscall(unsigned int entries, struct io_uring_params *p)
{
	return (int)syscall(SYS_io_uring_setup, entries, p);
}

static int io_uring_enter_syscall(int fd, unsigned int to_submit,
				   unsigned int min_complete,
				   unsigned int flags)
{
	return (int)syscall(SYS_io_uring_enter, fd, to_submit,
			    min_complete, flags, NULL, 0);
}

/* Set up an io_uring ring sized @depth with SQE128 (needed for ublk io cmds). */
static int ublk_ring_init(struct ublk_ring *r, int depth)
{
	struct io_uring_params p = {
		/* SQE128: each SQE is 128 bytes so the ublksrv_io_cmd fits */
		.flags = IORING_SETUP_SQE128 | IORING_SETUP_COOP_TASKRUN,
	};
	struct io_sqring_offsets *sq_off;
	struct io_cqring_offsets *cq_off;
	int ret;

	memset(r, 0, sizeof(*r));

	r->fd = io_uring_setup_syscall((unsigned int)depth, &p);
	if (r->fd < 0) {
		perror("io_uring_setup");
		return -1;
	}

	sq_off = &p.sq_off;
	cq_off = &p.cq_off;

	/* Map the SQ ring */
	r->sq_mmap_sz = sq_off->array + p.sq_entries * sizeof(unsigned int);
	r->sq_mmap = mmap(NULL, r->sq_mmap_sz, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
	if (r->sq_mmap == MAP_FAILED)
		goto err_close;

	r->sq_head    = (unsigned int *)((char *)r->sq_mmap + sq_off->head);
	r->sq_tail    = (unsigned int *)((char *)r->sq_mmap + sq_off->tail);
	r->sq_mask    = (unsigned int *)((char *)r->sq_mmap + sq_off->ring_mask);
	r->sq_array   = (unsigned int *)((char *)r->sq_mmap + sq_off->array);
	r->sq_entries = p.sq_entries;

	/*
	 * Map the SQEs.  With SQE128 each entry is 128 bytes, so the mmap
	 * size is sq_entries * 2 * sizeof(struct io_uring_sqe).
	 */
	r->sqes_mmap_sz = p.sq_entries * 2 * sizeof(struct io_uring_sqe);
	r->sqes_mmap = mmap(NULL, r->sqes_mmap_sz, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
	if (r->sqes_mmap == MAP_FAILED)
		goto err_unmap_sq;
	r->sqes = (struct io_uring_sqe *)r->sqes_mmap;

	/* Map the CQ ring */
	r->cq_mmap_sz = cq_off->cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	r->cq_mmap = mmap(NULL, r->cq_mmap_sz, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
	if (r->cq_mmap == MAP_FAILED)
		goto err_unmap_sqes;

	r->cq_head    = (unsigned int *)((char *)r->cq_mmap + cq_off->head);
	r->cq_tail    = (unsigned int *)((char *)r->cq_mmap + cq_off->tail);
	r->cq_mask    = (unsigned int *)((char *)r->cq_mmap + cq_off->ring_mask);
	r->cqes       = (struct io_uring_cqe *)((char *)r->cq_mmap + cq_off->cqes);
	r->cq_entries = p.cq_entries;

	ret = 0;
	goto out;

err_unmap_sqes:
	munmap(r->sqes_mmap, r->sqes_mmap_sz);
err_unmap_sq:
	munmap(r->sq_mmap, r->sq_mmap_sz);
err_close:
	close(r->fd);
	ret = -1;
out:
	return ret;
}

static void ublk_ring_exit(struct ublk_ring *r)
{
	munmap(r->cq_mmap, r->cq_mmap_sz);
	munmap(r->sqes_mmap, r->sqes_mmap_sz);
	munmap(r->sq_mmap, r->sq_mmap_sz);
	close(r->fd);
}

/*
 * Get a pointer to SQE slot @idx (accounting for the 128-byte stride).
 * Caller fills in the fields, then calls ublk_ring_submit().
 */
static struct io_uring_sqe *ublk_ring_get_sqe(struct ublk_ring *r)
{
	unsigned int tail = *r->sq_tail;
	unsigned int idx  = tail & *r->sq_mask;

	/* Identity mapping: sq_array[i] = i (set once, never changes) */
	r->sq_array[idx] = idx;

	/* With SQE128 the real stride is 128 B; cast accordingly */
	struct io_uring_sqe *sqe =
		(struct io_uring_sqe *)((char *)r->sqes + idx * 128);

	memset(sqe, 0, 128);

	/* Advance the tail so the kernel sees this SQE on the next enter */
	__atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
	r->sq_pending++;

	return sqe;
}

/* Submit all pending SQEs to the kernel (no wait). */
static int ublk_ring_submit(struct ublk_ring *r)
{
	int ret;

	if (!r->sq_pending)
		return 0;

	ret = io_uring_enter_syscall(r->fd, r->sq_pending, 0, 0);
	if (ret < 0) {
		perror("io_uring_enter (submit)");
		return -1;
	}
	r->sq_pending = 0;
	return ret;
}

/*
 * Submit pending SQEs and wait for at least @min_cqes CQEs.
 * Pass min_cqes=0 to submit without blocking.
 */
static int ublk_ring_submit_and_wait(struct ublk_ring *r, unsigned int min_cqes)
{
	unsigned int flags = min_cqes ? IORING_ENTER_GETEVENTS : 0;
	int ret;

	ret = io_uring_enter_syscall(r->fd, r->sq_pending, min_cqes, flags);
	if (ret < 0) {
		if (errno == EINTR)
			return 0;
		perror("io_uring_enter (wait)");
		return -1;
	}
	r->sq_pending = 0;
	return ret;
}

/* Peek at up to @max CQEs without blocking.  Returns number found. */
static int ublk_ring_peek_cqes(struct ublk_ring *r,
				struct io_uring_cqe *out, int max)
{
	unsigned int head = *r->cq_head;
	unsigned int tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
	int n = 0;

	while (head != tail && n < max) {
		out[n++] = r->cqes[head & *r->cq_mask];
		head++;
	}
	/* Advance head to consume the entries we just read */
	if (n)
		__atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);

	return n;
}

/* =========================================================================
 * Section 2: ublk device control — device paths and control functions
 *
 * Control functions are defined after Section 3 (struct definitions) because
 * they access struct unvme_ublk_dev fields.  See "Section 2 (cont.)" below.
 * ========================================================================= */

#define UBLK_CTRL_DEV  "/dev/ublk-control"
#define UBLK_CDEV_FMT  "/dev/ublkc%d"

/* =========================================================================
 * Section 3: Per-slot and per-queue structures
 * ========================================================================= */

/*
 * The io_cmd_buf for queue @qid lives in /dev/ublkc{N} at:
 *   offset = qid * round_up(UBLK_MAX_QUEUE_DEPTH * sizeof(ublksrv_io_desc), PAGE_SIZE)
 *   size   = round_up(queue_depth * sizeof(ublksrv_io_desc), PAGE_SIZE)
 *
 * The mapping must be PROT_READ only — the kernel writes into it and
 * ublk_ch_mmap() returns EPERM if VM_WRITE is set.
 */
#define UBLK_PAGE_SIZE  4096UL

static inline size_t ublk_cmd_buf_sz(int queue_depth)
{
	size_t raw = (size_t)queue_depth * sizeof(struct ublksrv_io_desc);
	return (raw + UBLK_PAGE_SIZE - 1) & ~(UBLK_PAGE_SIZE - 1);
}

static inline off_t ublk_cmd_buf_off(int qid)
{
	/* Stride = UBLK_MAX_QUEUE_DEPTH * 24 bytes = 98304 (already page-aligned) */
	size_t per_q = UBLK_MAX_QUEUE_DEPTH * sizeof(struct ublksrv_io_desc);
	return (off_t)(UBLKSRV_CMD_BUF_OFFSET + (size_t)qid * per_q);
}

/*
 * One in-flight I/O request tracked by the worker thread.
 *
 * There are queue_depth slots per queue.  A slot is FREE when it holds no
 * active request.  It becomes IN_FLIGHT after we submit the NVMe command,
 * and returns to FREE after we commit the ublk result.
 *
 * We map slot index → NVMe CID 1:1.  This lets us find the slot from a
 * NVMe CQE without any hash or search: slot = &q->slots[cqe.cid].
 */
struct ublk_slot {
	uint16_t  ublk_tag;   /* ublk request tag this slot is serving */
	bool      in_flight;  /* true: NVMe cmd submitted, completion pending */

	/*
	 * cmd: the libunvmed command handle for the in-flight NVMe operation.
	 * Stored here so we can call unvmed_cmd_put() in the completion path
	 * without a second lookup.
	 */
	struct unvme_cmd *cmd;

	/*
	 * dma_buf: page-aligned, IOMMU-mapped via unvmed_pgmap().
	 * The NVMe device DMA's directly into/from this buffer.
	 */
	void     *dma_buf;

	/*
	 * ublk_buf: mmap from /dev/ublkc{dev_id}.
	 * The kernel fills this with write data; we fill it with read data.
	 * One buffer per tag, mapped at startup.
	 */
	void     *ublk_buf;
};

/* Forward declaration */
struct unvme_ublk_dev;

/*
 * Per-queue worker state.  One instance per ublk/NVMe queue pair.
 * All fields are owned exclusively by the worker thread after init.
 */
struct unvme_ublk_queue {
	int                    qid;         /* queue index (0-based) */
	struct unvme_ublk_dev *dev;         /* back-pointer to device */

	/* NVMe queue pair created via libunvmed */
	struct unvme_sq       *usq;
	struct unvme_cq       *ucq;

	/* io_uring ring for ublk I/O */
	struct ublk_ring       ring;
	int                    cdev_fd;     /* /dev/ublkc{dev_id} */

	/*
	 * io_cmd_buf: mmap from cdev_fd at offset 0.
	 * The kernel writes ublksrv_io_desc here when delivering a request.
	 * Indexed by ublk tag: iod = &cmd_buf[tag].
	 */
	struct ublksrv_io_desc *cmd_buf;

	/* Slot pool — one slot per in-flight tag */
	int              nr_slots;    /* == queue_depth */
	struct ublk_slot *slots;

	/* Worker thread */
	pthread_t  thread;
	bool       stop;       /* set to true to request clean shutdown */
	bool       started;    /* true once thread is past init */
};

/* Top-level device handle (returned by unvmed_ublk_start). */
struct unvme_ublk_dev {
	struct unvme          *u;
	uint32_t               nsid;
	int                    dev_id;
	int                    nr_queues;
	int                    queue_depth;
	int                    base_sqid;   /* first NVMe queue ID used */
	uint32_t               max_io_buf_bytes;
	uint32_t               lba_size;   /* namespace logical block size in bytes */

	int                    ctrl_fd;    /* /dev/ublk-control */
	struct ublk_ring       ctrl_ring;  /* io_uring ring for control commands */

	struct unvme_ublk_queue *queues;   /* [nr_queues] */
};

/* =========================================================================
 * Section 2 (cont.): ublk device control functions
 *
 * Defined here (after struct unvme_ublk_dev) because they access struct
 * fields.  /dev/ublk-control has no ioctl() handler — all ctrl commands
 * must be submitted as IORING_OP_URING_CMD via the ctrl ring (SQE128).
 * The command opcode goes in sqe->cmd_op; the ublksrv_ctrl_cmd payload
 * goes into sqe->cmd[] (the extra 64 bytes of a 128-byte SQE).
 * ========================================================================= */

static int ublk_ctrl_uring_cmd(struct ublk_ring *ring, int ctrl_fd,
				unsigned int cmd_op,
				struct ublksrv_ctrl_cmd *payload)
{
	struct io_uring_sqe *sqe = ublk_ring_get_sqe(ring);
	struct io_uring_cqe cqe;
	int ret;

	sqe->opcode    = IORING_OP_URING_CMD;
	sqe->fd        = ctrl_fd;
	sqe->cmd_op    = cmd_op;
	sqe->user_data = 0;
	memcpy((void *)sqe->cmd, payload, sizeof(*payload));

	ret = ublk_ring_submit_and_wait(ring, 1);
	if (ret < 0)
		return ret;

	if (ublk_ring_peek_cqes(ring, &cqe, 1) < 1)
		return -EIO;

	return cqe.res;
}

static int ublk_add_dev(struct unvme_ublk_dev *dev)
{
	struct ublksrv_ctrl_dev_info info = {
		.nr_hw_queues     = (uint16_t)dev->nr_queues,
		.queue_depth      = (uint16_t)dev->queue_depth,
		.max_io_buf_bytes = dev->max_io_buf_bytes,
		.dev_id           = (uint32_t)dev->dev_id,
		.flags            = UBLK_F_CMD_IOCTL_ENCODE,
	};
	struct ublksrv_ctrl_cmd cmd = {
		.dev_id   = (uint32_t)dev->dev_id,
		.queue_id = (__u16)-1,
		.len      = sizeof(info),
		.addr     = (uintptr_t)&info,
	};

	return ublk_ctrl_uring_cmd(&dev->ctrl_ring, dev->ctrl_fd,
				   UBLK_U_CMD_ADD_DEV, &cmd);
}

static int ublk_set_params(struct unvme_ublk_dev *dev,
			    uint64_t nr_sectors, uint8_t lba_shift)
{
	struct ublk_params params = {
		.len   = sizeof(params),
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift  = lba_shift,
			.physical_bs_shift = lba_shift,
			.io_opt_shift      = lba_shift,
			.io_min_shift      = lba_shift,
			.dev_sectors       = nr_sectors,
		},
	};
	struct ublksrv_ctrl_cmd cmd = {
		.dev_id   = (uint32_t)dev->dev_id,
		.queue_id = (__u16)-1,
		.len      = sizeof(params),
		.addr     = (uintptr_t)&params,
	};

	return ublk_ctrl_uring_cmd(&dev->ctrl_ring, dev->ctrl_fd,
				   UBLK_U_CMD_SET_PARAMS, &cmd);
}

static int ublk_start_dev(struct unvme_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {
		.dev_id   = (uint32_t)dev->dev_id,
		.queue_id = (__u16)-1,
		.data[0]  = getpid(),
	};

	return ublk_ctrl_uring_cmd(&dev->ctrl_ring, dev->ctrl_fd,
				   UBLK_U_CMD_START_DEV, &cmd);
}

static int ublk_stop_dev(struct unvme_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {
		.dev_id   = (uint32_t)dev->dev_id,
		.queue_id = (__u16)-1,
	};

	return ublk_ctrl_uring_cmd(&dev->ctrl_ring, dev->ctrl_fd,
				   UBLK_U_CMD_STOP_DEV, &cmd);
}

static int ublk_del_dev(struct unvme_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {
		.dev_id   = (uint32_t)dev->dev_id,
		.queue_id = (__u16)-1,
	};

	return ublk_ctrl_uring_cmd(&dev->ctrl_ring, dev->ctrl_fd,
				   UBLK_U_CMD_DEL_DEV, &cmd);
}

/* =========================================================================
 * Section 4: ublk I/O descriptor helpers
 * ========================================================================= */

/* Return a pointer to the I/O descriptor for the given tag in this queue. */
static inline const struct ublksrv_io_desc *
ublk_get_iod(struct unvme_ublk_queue *q, uint16_t tag)
{
	return &q->cmd_buf[tag];
}

/*
 * Prepare and enqueue a FETCH_REQ SQE for the given tag.
 *
 * We tell the kernel: "please deliver the next request for this tag into
 * ublk_buf[tag]".  The CQE that comes back indicates a request is ready.
 */
static void ublk_queue_fetch_req(struct unvme_ublk_queue *q, uint16_t tag)
{
	struct ublk_slot   *slot = &q->slots[tag];
	struct io_uring_sqe *sqe  = ublk_ring_get_sqe(&q->ring);

	/*
	 * ublksrv_io_cmd is placed in sqe->cmd[] (the extra 64 bytes of a
	 * 128-byte SQE).  We cast to the struct for readability.
	 */
	struct ublksrv_io_cmd *io_cmd =
		(struct ublksrv_io_cmd *)sqe->cmd;

	sqe->opcode   = IORING_OP_URING_CMD;
	sqe->fd       = q->cdev_fd;
	sqe->cmd_op   = UBLK_U_IO_FETCH_REQ;
	sqe->user_data = tag;   /* echo'd back in the CQE so we know the tag */

	io_cmd->q_id   = (__u16)q->qid;
	io_cmd->tag    = tag;
	io_cmd->result = 0;
	/* Tell the kernel where to place write data (our ublk buffer) */
	io_cmd->addr   = (uint64_t)(uintptr_t)slot->ublk_buf;
}

/*
 * Prepare and enqueue a COMMIT_AND_FETCH_REQ SQE.
 *
 * This tells the kernel: "the I/O for tag @tag is done with result @result,
 * and please deliver the next request for this tag".
 *
 * result = 0 means success.  Negative errno values signal errors to the
 * block layer (e.g., -EIO).
 */
static void ublk_queue_commit_req(struct unvme_ublk_queue *q,
				   uint16_t tag, int32_t result)
{
	struct ublk_slot   *slot = &q->slots[tag];
	struct io_uring_sqe *sqe  = ublk_ring_get_sqe(&q->ring);
	struct ublksrv_io_cmd *io_cmd =
		(struct ublksrv_io_cmd *)sqe->cmd;

	sqe->opcode   = IORING_OP_URING_CMD;
	sqe->fd       = q->cdev_fd;
	sqe->cmd_op   = UBLK_U_IO_COMMIT_AND_FETCH_REQ;
	sqe->user_data = tag;

	io_cmd->q_id   = (__u16)q->qid;
	io_cmd->tag    = tag;
	io_cmd->result = result;
	/* Also pass the buffer address for the next fetch */
	io_cmd->addr   = (uint64_t)(uintptr_t)slot->ublk_buf;
}

/* =========================================================================
 * Section 5: NVMe command submission
 *
 * We use the lower-level libunvmed primitives (alloc_cmd + cmd_post) rather
 * than the blocking unvmed_read/write() so we can pipeline multiple commands.
 * ========================================================================= */

/*
 * Submit a NVMe read or write for slot @slot_idx without ringing the
 * doorbell (UNVMED_CMD_F_NODB).  The caller batches multiple commands and
 * rings the doorbell once with unvmed_sq_update_tail().
 *
 * We use slot_idx as the NVMe CID so that completions can be matched back
 * to slots without any look-up: slot = &q->slots[cqe.cid].
 */
static int ublk_submit_nvme(struct unvme_ublk_queue *q, int slot_idx,
			     bool is_write, uint64_t start_sector,
			     uint32_t nr_sectors)
{
	struct unvme_ublk_dev *dev = q->dev;
	struct ublk_slot      *slot = &q->slots[slot_idx];
	struct unvme_cmd      *cmd;
	struct nvme_cmd_rw    *sqe;
	struct iovec           iov;
	uint64_t               slba;
	uint16_t               nlb;   /* 0-based: (nr_sectors in lba units) - 1 */
	size_t                 io_bytes = (size_t)nr_sectors * 512;

	/*
	 * ublk sectors are always 512 B; convert to NVMe LBAs using the
	 * namespace's lba_size (stored at device init time from unvme_ns).
	 */
	slba = start_sector / (dev->lba_size / 512);
	nlb  = (uint16_t)(nr_sectors / (dev->lba_size / 512) - 1);  /* 0-based */

	iov.iov_base = slot->dma_buf;
	iov.iov_len  = io_bytes;

	/*
	 * Allocate a NVMe command pinning CID to slot_idx.  This lets us
	 * recover the slot in O(1) from a NVMe CQE (cqe.cid == slot_idx).
	 */
	uint16_t cid = (uint16_t)slot_idx;
	cmd = unvmed_alloc_cmd(dev->u, q->usq, &cid,
			       slot->dma_buf, io_bytes);
	if (!cmd) {
		unvmed_log_err("ublk[%d/%d]: unvmed_alloc_cmd failed",
			       dev->dev_id, q->qid);
		return -ENOMEM;
	}

	/*
	 * Fill the NVMe SQE via cmd->sqe — the same pattern used by
	 * libunvmed-cmds.c (unvmed_cmd_prep_read / _prep_write).
	 */
	sqe = (struct nvme_cmd_rw *)&cmd->sqe;
	sqe->opcode = is_write ? nvme_cmd_write : nvme_cmd_read;
	sqe->nsid   = cpu_to_le32(dev->nsid);
	sqe->slba   = cpu_to_le64(slba);
	sqe->nlb    = cpu_to_le16(nlb);
	sqe->cid    = cmd->cid;

	/* Set up PRP data pointer entries in the SQE */
	if (__unvmed_mapv_prp(cmd, &cmd->sqe, &iov, 1) < 0) {
		unvmed_log_err("ublk[%d/%d]: PRP mapping failed",
			       dev->dev_id, q->qid);
		unvmed_cmd_put(cmd);
		return -ENOMEM;
	}

	/* Post to SQ without ringing the doorbell — caller batches and rings once */
	unvmed_cmd_post(cmd, &cmd->sqe, UNVMED_CMD_F_NODB);

	slot->cmd       = cmd;
	slot->in_flight = true;

	return 0;
}

/* =========================================================================
 * Section 6: Worker thread — the main event loop
 * ========================================================================= */

#define MAX_CQE_BATCH  64   /* max CQEs processed per loop iteration */

static void *ublk_worker(void *arg)
{
	struct unvme_ublk_queue *q   = (struct unvme_ublk_queue *)arg;
	struct unvme_ublk_dev   *dev = q->dev;
	struct unvme            *u   = dev->u;

	struct io_uring_cqe  ublk_cqes[MAX_CQE_BATCH];
	struct nvme_cqe      nvme_cqes[MAX_CQE_BATCH];
	int nr_ublk, nr_nvme;
	int nr_submitted;   /* NVMe cmds posted since last doorbell */
	int i;

	unvmed_log_info("ublk[%d/%d]: worker started", dev->dev_id, q->qid);
	q->started = true;

	/*
	 * Prime the pump: issue one FETCH_REQ for every slot so the kernel
	 * can immediately deliver I/O requests as they arrive.
	 */
	for (int tag = 0; tag < q->nr_slots; tag++)
		ublk_queue_fetch_req(q, (uint16_t)tag);
	ublk_ring_submit(&q->ring);

	while (!q->stop) {
		nr_submitted = 0;

		/* ---- Step A: collect incoming ublk I/O requests ---------- */

		nr_ublk = ublk_ring_peek_cqes(&q->ring, ublk_cqes, MAX_CQE_BATCH);

		for (i = 0; i < nr_ublk; i++) {
			uint16_t tag    = (uint16_t)ublk_cqes[i].user_data;
			int32_t  res    = ublk_cqes[i].res;
			struct ublk_slot *slot = &q->slots[tag];
			const struct ublksrv_io_desc *iod = ublk_get_iod(q, tag);
			uint8_t  op = ublksrv_get_op(iod);
			bool     is_write;

			/* res < 0 means the device is being torn down */
			if (res < 0) {
				if (res != -ENODEV)
					unvmed_log_err("ublk[%d/%d]: fetch_req tag=%u res=%d",
						dev->dev_id, q->qid, tag, res);
				q->stop = true;
				break;
			}

			if (op == UBLK_IO_OP_READ) {
				is_write = false;
			} else if (op == UBLK_IO_OP_WRITE) {
				is_write = true;
				/*
				 * For WRITE: the kernel already placed the data
				 * in slot->ublk_buf.  Copy it into our IOMMU-
				 * mapped DMA buffer before submitting to NVMe.
				 */
				memcpy(slot->dma_buf, slot->ublk_buf,
				       (size_t)iod->nr_sectors * 512);
			} else {
				/*
				 * Unsupported op (FLUSH, DISCARD, …).
				 * Return success without touching NVMe — a
				 * proper implementation would handle these.
				 */
				ublk_queue_commit_req(q, tag, 0);
				continue;
			}

			slot->ublk_tag = tag;
			unvmed_sq_enter(q->usq);
			if (ublk_submit_nvme(q, tag, is_write,
					     iod->start_sector,
					     iod->nr_sectors) < 0) {
				unvmed_sq_exit(q->usq);
				/* Report I/O error back to the block layer */
				ublk_queue_commit_req(q, tag, -EIO);
				continue;
			}
			unvmed_sq_exit(q->usq);
			nr_submitted++;
		}

		/* ---- Step B: ring the NVMe doorbell once for the batch --- */

		if (nr_submitted) {
			unvmed_sq_enter(q->usq);
			unvmed_sq_update_tail(u, q->usq);
			unvmed_sq_exit(q->usq);
		}

		/* ---- Step C: poll NVMe CQ for completions (non-blocking) - */

		nr_nvme = __unvmed_cq_run_n(u, q->usq, q->ucq,
					     NULL,   /* use usq->vcq */
					     nvme_cqes, MAX_CQE_BATCH,
					     true);  /* nowait */

		for (i = 0; i < nr_nvme; i++) {
			/*
			 * We used slot_idx as the NVMe CID, so recovering the
			 * slot is O(1): just index by cid.
			 */
			uint16_t cid  = nvme_cqes[i].cid;
			struct ublk_slot *slot = &q->slots[cid];
			int      status = unvmed_cqe_status(&nvme_cqes[i]);
			int32_t  result = (status == 0) ? 0 : -EIO;
			uint8_t  op;

			if (!slot->in_flight) {
				unvmed_log_err("ublk[%d/%d]: stale CQE cid=%u",
					dev->dev_id, q->qid, cid);
				continue;
			}

			op = ublksrv_get_op(ublk_get_iod(q, cid));
			if (op == UBLK_IO_OP_READ && result == 0) {
				/*
				 * For READ: copy from the NVMe DMA buffer into
				 * the ublk buffer so the kernel can return the
				 * data to the application.
				 */
				const struct ublksrv_io_desc *iod =
					ublk_get_iod(q, cid);
				memcpy(slot->ublk_buf, slot->dma_buf,
				       (size_t)iod->nr_sectors * 512);
			}

			/*
			 * Release the cmd back to the pool using the pointer
			 * we saved at submission time (avoids a second lookup).
			 */
			unvmed_cmd_put(slot->cmd);
			slot->cmd = NULL;

			slot->in_flight = false;

			/* Commit this ublk request and ask for the next one */
			ublk_queue_commit_req(q, cid, result);
		}

		/*
		 * ---- Step D: submit COMMIT_AND_FETCH SQEs and, if we have
		 * nothing in flight, sleep until the kernel sends more work.
		 * ---------------------------------------------------------- */

		bool any_in_flight = false;
		for (int s = 0; s < q->nr_slots; s++) {
			if (q->slots[s].in_flight) {
				any_in_flight = true;
				break;
			}
		}

		if (any_in_flight) {
			/* Non-blocking submit; stay in the loop to poll NVMe */
			ublk_ring_submit(&q->ring);
		} else {
			/*
			 * Nothing in flight: block until the kernel delivers
			 * at least one new I/O request (or we're stopped).
			 */
			ublk_ring_submit_and_wait(&q->ring, 1);
		}
	}

	unvmed_log_info("ublk[%d/%d]: worker stopping", dev->dev_id, q->qid);
	return NULL;
}

/* =========================================================================
 * Section 7: Queue setup and teardown
 * ========================================================================= */

static int ublk_queue_init(struct unvme_ublk_queue *q,
			    struct unvme_ublk_dev *dev, int qid)
{
	struct unvme *u = dev->u;
	char cdev_path[64];
	int sqid = dev->base_sqid + qid;
	int ret;

	q->qid    = qid;
	q->dev    = dev;
	q->stop   = false;
	q->started = false;

	/*
	 * Create the NVMe I/O queue pair.
	 * CQ must be created before SQ (NVMe spec requirement).
	 * vector=0 → no MSI-X interrupt; libunvmed will poll the CQ ring.
	 * pc=1 → physically contiguous queue memory (standard setting).
	 */
	q->ucq = unvmed_init_cq(u, sqid, dev->queue_depth + 1, 0 /* vector */,
				1 /* pc */);
	if (!q->ucq) {
		unvmed_log_err("ublk[%d/%d]: failed to init CQ (cqid=%d)",
			dev->dev_id, qid, sqid);
		return -1;
	}
	unvmed_enable_cq(q->ucq);

	q->usq = unvmed_init_sq(u, sqid, dev->queue_depth + 1, sqid,
				0 /* qprio */, 1 /* pc */, 0 /* nvmsetid */);
	if (!q->usq) {
		unvmed_log_err("ublk[%d/%d]: failed to init SQ (sqid=%d)",
			dev->dev_id, qid, sqid);
		goto err_del_cq;
	}
	unvmed_enable_sq(q->usq);

	/* Open the ublk character device for this queue's io_uring */
	snprintf(cdev_path, sizeof(cdev_path), UBLK_CDEV_FMT, dev->dev_id);
	q->cdev_fd = open(cdev_path, O_RDWR);
	if (q->cdev_fd < 0) {
		unvmed_log_err("ublk[%d/%d]: open(%s): %s",
			dev->dev_id, qid, cdev_path, strerror(errno));
		goto err_del_sq;
	}

	/*
	 * Map the io_cmd_buf from /dev/ublkc{N}.
	 * - PROT_READ only: ublk_ch_mmap() returns EPERM if VM_WRITE is set;
	 *   the kernel writes descriptors here and the server only reads them.
	 * - Size must be page-rounded (kernel validates exact match).
	 * - Offset = qid * stride, where stride = UBLK_MAX_QUEUE_DEPTH * 24.
	 */
	size_t cmd_buf_sz = ublk_cmd_buf_sz(dev->queue_depth);
	q->cmd_buf = mmap(NULL, cmd_buf_sz, PROT_READ,
			  MAP_SHARED | MAP_POPULATE,
			  q->cdev_fd, ublk_cmd_buf_off(qid));
	if (q->cmd_buf == MAP_FAILED) {
		unvmed_log_err("ublk[%d/%d]: mmap cmd_buf: %s",
			dev->dev_id, qid, strerror(errno));
		goto err_close_cdev;
	}

	/* Allocate the slot pool */
	q->nr_slots = dev->queue_depth;
	q->slots = calloc(q->nr_slots, sizeof(*q->slots));
	if (!q->slots)
		goto err_unmap_cmdbuf;

	/* Per-slot buffer setup */
	for (int tag = 0; tag < q->nr_slots; tag++) {
		struct ublk_slot *slot = &q->slots[tag];

		/* --- NVMe DMA buffer --- */
		ret = (int)unvmed_pgmap(u, &slot->dma_buf,
					dev->max_io_buf_bytes);
		if (ret < 0) {
			unvmed_log_err("ublk[%d/%d]: unvmed_pgmap failed tag=%d",
				dev->dev_id, qid, tag);
			/* clean up already-allocated slots */
			for (int j = 0; j < tag; j++) {
				unvmed_unmap_vaddr(u, q->slots[j].dma_buf);
				munmap(q->slots[j].ublk_buf,
				       dev->max_io_buf_bytes);
			}
			goto err_free_slots;
		}

		/*
		 * --- ublk I/O buffer ---
		 * The kernel places each tag's buffer at a well-known offset in
		 * the char device mmap space:
		 *
		 *   offset = UBLKSRV_IO_BUF_OFFSET
		 *            | ((uint64_t)qid << UBLK_QID_OFF)
		 *            | ((uint64_t)tag << UBLK_TAG_OFF)
		 *
		 * The individual buf size is max_io_buf_bytes.
		 */
		uint64_t ublk_buf_off =
			UBLKSRV_IO_BUF_OFFSET |
			((uint64_t)qid << UBLK_QID_OFF) |
			((uint64_t)tag << UBLK_TAG_OFF);

		slot->ublk_buf = mmap(NULL, dev->max_io_buf_bytes,
				      PROT_READ | PROT_WRITE,
				      MAP_SHARED | MAP_POPULATE,
				      q->cdev_fd, (off_t)ublk_buf_off);
		if (slot->ublk_buf == MAP_FAILED) {
			unvmed_log_err("ublk[%d/%d]: mmap ublk_buf tag=%d: %s",
				dev->dev_id, qid, tag, strerror(errno));
			unvmed_unmap_vaddr(u, slot->dma_buf);
			for (int j = 0; j < tag; j++) {
				unvmed_unmap_vaddr(u, q->slots[j].dma_buf);
				munmap(q->slots[j].ublk_buf,
				       dev->max_io_buf_bytes);
			}
			goto err_free_slots;
		}

		slot->in_flight = false;
	}

	/* Set up the io_uring ring (depth = queue_depth) */
	if (ublk_ring_init(&q->ring, dev->queue_depth) < 0)
		goto err_cleanup_slots;

	/* Spawn the worker thread */
	if (pthread_create(&q->thread, NULL, ublk_worker, q) != 0) {
		unvmed_log_err("ublk[%d/%d]: pthread_create failed",
			dev->dev_id, qid);
		ublk_ring_exit(&q->ring);
		goto err_cleanup_slots;
	}

	return 0;

err_cleanup_slots:
	for (int j = 0; j < q->nr_slots; j++) {
		unvmed_unmap_vaddr(u, q->slots[j].dma_buf);
		munmap(q->slots[j].ublk_buf, dev->max_io_buf_bytes);
	}
err_free_slots:
	free(q->slots);
	q->slots = NULL;
err_unmap_cmdbuf:
	munmap(q->cmd_buf, ublk_cmd_buf_sz(dev->queue_depth));
err_close_cdev:
	close(q->cdev_fd);
err_del_sq:
	unvmed_sq_put(u, q->usq);
err_del_cq:
	unvmed_cq_put(u, q->ucq);
	return -1;
}

static void ublk_queue_teardown(struct unvme_ublk_queue *q)
{
	struct unvme_ublk_dev *dev = q->dev;
	struct unvme          *u   = dev->u;
	size_t cmd_buf_sz = ublk_cmd_buf_sz(dev->queue_depth);

	/* Signal worker to exit and wait */
	q->stop = true;
	pthread_join(q->thread, NULL);

	ublk_ring_exit(&q->ring);

	for (int tag = 0; tag < q->nr_slots; tag++) {
		munmap(q->slots[tag].ublk_buf, dev->max_io_buf_bytes);
		unvmed_unmap_vaddr(u, q->slots[tag].dma_buf);
	}
	free(q->slots);

	munmap(q->cmd_buf, cmd_buf_sz);
	close(q->cdev_fd);

	unvmed_sq_put(u, q->usq);
	unvmed_cq_put(u, q->ucq);
}

/* =========================================================================
 * Section 8: Public API
 * ========================================================================= */

struct unvme_ublk_dev *unvmed_ublk_start(struct unvme *u, uint32_t nsid,
					  int dev_id, int nr_queues,
					  int queue_depth, int base_sqid)
{
	struct unvme_ublk_dev *dev;
	struct unvme_ns       *ns;
	ssize_t                max_xfer;
	int                    i;

	/* Validate the namespace exists */
	ns = unvmed_ns_get(u, nsid);
	if (!ns) {
		unvmed_log_err("ublk: namespace nsid=%u not found", nsid);
		return NULL;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		unvmed_ns_put(u, ns);
		return NULL;
	}

	dev->u           = u;
	dev->nsid        = nsid;
	dev->dev_id      = dev_id;
	dev->nr_queues   = nr_queues;
	dev->queue_depth = queue_depth;
	dev->base_sqid   = base_sqid;
	dev->lba_size    = ns->lba_size;   /* cached for I/O sector conversion */

	/*
	 * Use the controller's maximum transfer size as the I/O buffer size,
	 * capped at 1 MiB so we don't over-allocate DMA memory.
	 */
	max_xfer = unvmed_get_max_xfer_size(u);
	dev->max_io_buf_bytes = (uint32_t)((max_xfer > 0 && max_xfer < (1 << 20))
					    ? max_xfer : (1 << 20));

	/* Open the ublk control device */
	dev->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (dev->ctrl_fd < 0) {
		unvmed_log_err("ublk: open(%s): %s", UBLK_CTRL_DEV,
			strerror(errno));
		goto err_free_dev;
	}

	/*
	 * Set up a small io_uring ring (SQE128) for control commands.
	 * /dev/ublk-control has no ioctl() handler — all ctrl commands
	 * must be submitted as IORING_OP_URING_CMD via this ring.
	 */
	if (ublk_ring_init(&dev->ctrl_ring, 4) < 0) {
		unvmed_log_err("ublk: ctrl ring init failed");
		goto err_close_ctrl;
	}

	/* Create the ublk device — generates /dev/ublkc{dev_id} */
	if (ublk_add_dev(dev) < 0) {
		unvmed_log_err("ublk: UBLK_CMD_ADD_DEV failed (ret=%d)",
			errno);
		goto err_exit_ctrl_ring;
	}

	/*
	 * Configure block device geometry from the NVMe namespace.
	 * ublk sectors are 512 B; ns->nr_lbas × (lba_size/512) gives the
	 * total sector count.
	 */
	uint64_t nr_sectors =
		(uint64_t)ns->nr_lbas * (ns->lba_size / 512);
	uint8_t lba_shift = __builtin_ctz(ns->lba_size);  /* log2(lba_size) */

	if (ublk_set_params(dev, nr_sectors, lba_shift) < 0) {
		unvmed_log_err("ublk: UBLK_CMD_SET_PARAMS failed (ret=%d)",
			errno);
		goto err_del_dev;
	}

	/* Allocate and initialise per-queue structures */
	dev->queues = calloc(nr_queues, sizeof(*dev->queues));
	if (!dev->queues)
		goto err_del_dev;

	for (i = 0; i < nr_queues; i++) {
		if (ublk_queue_init(&dev->queues[i], dev, i) < 0) {
			unvmed_log_err("ublk: queue %d init failed", i);
			/* tear down already-initialised queues */
			for (int j = 0; j < i; j++)
				ublk_queue_teardown(&dev->queues[j]);
			goto err_free_queues;
		}
	}

	/*
	 * Start the ublk device — /dev/ublkb{dev_id} appears here.
	 * This must be called only after all per-queue io_urings have
	 * submitted their initial FETCH_REQs (done inside ublk_queue_init →
	 * ublk_worker startup).
	 */
	if (ublk_start_dev(dev) < 0) {
		unvmed_log_err("ublk: UBLK_CMD_START_DEV failed (ret=%d)",
			errno);
		for (i = 0; i < nr_queues; i++)
			ublk_queue_teardown(&dev->queues[i]);
		goto err_free_queues;
	}

	unvmed_ns_put(u, ns);
	unvmed_log_info("ublk: /dev/ublkb%d ready (%d queue(s), depth=%d)",
		dev_id, nr_queues, queue_depth);
	return dev;

err_free_queues:
	free(dev->queues);
err_del_dev:
	ublk_del_dev(dev);
err_exit_ctrl_ring:
	ublk_ring_exit(&dev->ctrl_ring);
err_close_ctrl:
	close(dev->ctrl_fd);
err_free_dev:
	unvmed_ns_put(u, ns);
	free(dev);
	return NULL;
}

void unvmed_ublk_stop(struct unvme_ublk_dev *dev)
{
	/* Ask the kernel to stop sending new I/O — drains the block queue */
	ublk_stop_dev(dev);

	/* Tear down each queue (signals worker thread + waits for it) */
	for (int i = 0; i < dev->nr_queues; i++)
		ublk_queue_teardown(&dev->queues[i]);

	/* Remove /dev/ublkb{dev_id} and /dev/ublkc{dev_id} */
	ublk_del_dev(dev);
	ublk_ring_exit(&dev->ctrl_ring);
	close(dev->ctrl_fd);

	free(dev->queues);
	free(dev);

	unvmed_log_info("ublk: device stopped");
}
