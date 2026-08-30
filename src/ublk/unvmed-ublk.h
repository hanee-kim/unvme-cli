/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNVMED_UBLK_H
#define UNVMED_UBLK_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>

#include <liburing.h>
#include <linux/ublk_cmd.h>

struct unvme;
struct unvme_sq;
struct unvme_cq;
struct unvme_msg;

#define UNVMED_UBLK_MAX_QUEUES       32
#define UNVMED_UBLK_DEF_NR_QUEUES   4
#define UNVMED_UBLK_DEF_DEPTH       64
#define UNVMED_UBLK_DEF_POLL_US     10   /* hybrid: spin 10 μs then yield */
#define UNVMED_UBLK_MAX_IO_SIZE     (512 * 1024)  /* bounce buf slot cap */

#define UBLK_CTRL_DEV               "/dev/ublk-control"

/*
 * Per-queue state.  One handler thread owns each queue exclusively;
 * no locking needed on the hot I/O path.
 */
struct unvmed_ublk_queue {
	struct unvmed_ublk_server  *server;
	int                         qid;

	/* io_uring ring for ublk IO commands (SQE128 mode) */
	struct io_uring             ring;

	/* per-queue ublk char device fd (/dev/ublkc<dev_id>) */
	int                         dev_fd;

	/* NVMe queue pair: 1:1 dedicated, lock-free hot path */
	struct unvme_sq            *usq;
	struct unvme_cq            *ucq;

	/*
	 * Bounce buffer: one contiguous IOMMU-mapped region split into
	 * queue_depth slots of slot_size bytes each.
	 *
	 * Double role:
	 *   1. Provided to ublk kernel as cmd->addr so the kernel can
	 *      copy_to/from_user write/read data here.
	 *   2. Used as NVMe DMA source/target (IOMMU-mapped via libvfn).
	 */
	void                       *bounce;       /* unvmed_pgmap() + map_vaddr */
	size_t                      bounce_total; /* nr_slots * slot_size */
	size_t                      slot_size;    /* max bytes per I/O */

	/*
	 * cmd_buf: mmap of the ublk char device's UBLKSRV_CMD_BUF_OFFSET
	 * region for this queue.  Contains ublksrv_io_desc[queue_depth]
	 * written by the kernel on each FETCH_REQ completion.
	 */
	struct ublksrv_io_desc     *io_descs;     /* mmap base for this queue */
	size_t                      io_descs_size;/* bytes mmap'd */

	/* inflight NVMe command indexed by ublk tag */
	struct unvme_cmd          **inflight;     /* [queue_depth] */

	uint32_t                    poll_spin_us;

	pthread_t                   thread;
	volatile bool               running;

	/*
	 * Signalled by the handler thread after the initial FETCH_REQs have
	 * been submitted via io_uring_submit_and_wait().  The server start
	 * path waits on this before issuing UBLK_CMD_START_DEV so the kernel's
	 * ublk_is_ready() check sees all queues prepared.
	 */
	sem_t                       fetch_submitted;
};

/*
 * Top-level ublk server: manages the ublk device lifecycle and
 * coordinates all per-queue handler threads.
 */
struct unvmed_ublk_server {
	struct unvme               *u;
	int                         ctrl_fd;     /* /dev/ublk-control */
	int                         dev_id;      /* assigned by kernel (ADD_DEV) */

	/*
	 * io_uring ring for control commands (ADD_DEV, SET_PARAMS, etc.).
	 * UBLK_U_CMD_* are cmd_op values for io_uring, not ioctl numbers.
	 * Must use SQE128 since struct ublksrv_ctrl_cmd (32 bytes) is
	 * embedded in sqe->cmd[] which is only 16 bytes in a standard SQE.
	 */
	struct io_uring             ctrl_ring;

	uint32_t                    nr_queues;
	uint32_t                    queue_depth;
	uint32_t                    nsid;
	uint64_t                    nr_sectors;  /* from namespace */
	uint32_t                    lba_size;    /* from namespace (bytes) */
	size_t                      max_io_size; /* min(MDTS, UNVMED_UBLK_MAX_IO_SIZE) */

	struct unvmed_ublk_queue   *queues[UNVMED_UBLK_MAX_QUEUES];

	volatile bool               running;

	/* first NVMe qid allocated for this server (ublk queues: base_qid..base_qid+nr_queues-1) */
	uint32_t                    base_qid;
};

/*
 * Start a ublk server on top of the given unvme controller.
 *
 * Creates /dev/ublkb<id>, allocates NVMe I/O queues, pre-maps bounce
 * buffers, starts per-queue handler threads, and makes the block device
 * live.  The server runs in the background; caller must call
 * unvmed_ublk_server_stop() to tear it down.
 *
 * Returns the server handle on success, NULL on error with errno set.
 */
struct unvmed_ublk_server *unvmed_ublk_server_start(struct unvme *u,
						    uint32_t nsid,
						    uint32_t nr_queues,
						    uint32_t queue_depth,
						    uint32_t poll_spin_us);

/*
 * Stop the server: signals handler threads, drains in-flight NVMe commands,
 * issues STOP_DEV / DEL_DEV, unmaps buffers, frees all resources.
 */
int unvmed_ublk_server_stop(struct unvmed_ublk_server *server);

/* Internal: per-queue thread entry point (called from unvmed-ublk.c) */
void *unvmed_ublk_queue_handler(void *arg);

/*
 * Stop all active ublk servers.  Called from the SIGTERM handler before
 * NVMe controller teardown so that queue handler threads exit cleanly and
 * kernel ublk state is properly released, preventing D-state processes.
 */
void unvmed_ublk_stop_all_servers(void);

#endif /* UNVMED_UBLK_H */
