/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNVMED_UBLK_H
#define UNVMED_UBLK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>

#include <liburing.h>
#include <linux/ublk_cmd.h>

/* Not yet in older kernel uapi headers; value from include/uapi/linux/ublk_cmd.h */
#ifndef UBLK_F_NO_AUTO_PART_SCAN
#define UBLK_F_NO_AUTO_PART_SCAN	(1ULL << 18)
#endif

struct unvme;
struct unvme_sq;
struct unvme_cq;
struct unvme_msg;

#define UNVMED_UBLK_MAX_SERVERS      8
#define UNVMED_UBLK_MAX_QUEUES       32
#define UNVMED_UBLK_DEF_DEPTH       64
#define UNVMED_UBLK_DEF_POLL_US     10   /* hybrid: spin 10 μs then yield */
#define UNVMED_UBLK_MAX_IO_SIZE     (512 * 1024)  /* bounce buf slot cap */

#define UBLK_CTRL_DEV               "/dev/ublk-control"

/*
 * Per-queue state.  One handler thread owns each ublk queue, which maps 1:1
 * to an existing NVMe CQ.  Multiple NVMe SQs may share that CQ; submissions
 * are round-robined across usqs[0..nr_usqs-1].
 *
 * The server does NOT create new NVMe queues — it reuses the I/O CQs (and
 * their associated SQs) that the user created before starting the server.
 * All those SQs are marked UNVMED_SQ_F_UBLK_OWNED while the server runs,
 * preventing direct I/O commands from racing with ublk submissions.
 */
struct unvmed_ublk_queue {
	struct unvmed_ublk_server  *server;
	int                         qid;

	/*
	 * io_uring ring for ublk IO commands (SQE128 mode).  Created and
	 * destroyed by the handler thread (IORING_SETUP_SINGLE_ISSUER).
	 */
	struct io_uring             ring;
	bool                        ring_ready;
	/*
	 * true if the ring was set up with IORING_SETUP_TASKRUN_FLAG, i.e.
	 * the kernel raises IORING_SQ_TASKRUN when ublk task-work is pending
	 * and the handler can skip io_uring_enter(2) while it is clear.
	 */
	bool                        taskrun_flag;

	/* per-queue ublk char device fd (/dev/ublkc<dev_id>) */
	int                         dev_fd;

	/* NVMe CQ/SQ pair reused from user-created queues (1:1). */
	struct unvme_cq            *ucq;
	struct unvme_sq            *usq;

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
	uint64_t                    bounce_iova;  /* cached IOVA of bounce[0] */
	size_t                      bounce_total; /* nr_slots * slot_size */
	size_t                      slot_size;    /* max bytes per I/O */

	/*
	 * Pre-built PRP lists, one controller page per tag.  Slot IOVAs are
	 * fixed for the lifetime of the queue, so the PRP list describing a
	 * full slot never changes; I/Os spanning more than two pages simply
	 * point PRP2 at their tag's list.  NULL if a slot fits in two pages.
	 */
	void                       *prplists;
	uint64_t                    prplists_iova;
	size_t                      page_size;    /* controller MPS in bytes */

	/*
	 * cmd_buf: mmap of the ublk char device's UBLKSRV_CMD_BUF_OFFSET
	 * region for this queue.  Contains ublksrv_io_desc[queue_depth]
	 * written by the kernel on each FETCH_REQ completion.
	 */
	struct ublksrv_io_desc     *io_descs;     /* mmap base for this queue */
	size_t                      io_descs_size;/* bytes mmap'd */

	uint32_t                    poll_spin_us;

	pthread_t                   thread;
	_Atomic bool                running;

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
 *
 * Queues reuse existing user-created NVMe I/O CQs (and their associated SQs).
 * One handler thread is started per existing I/O CQ.
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

	_Atomic bool                running;
};

/*
 * Start a ublk server on top of the given unvme controller.
 *
 * Reuses all existing user-created NVMe I/O CQs (and their SQs) — one
 * handler thread per CQ.  All reused SQs are marked UNVMED_SQ_F_UBLK_OWNED
 * to block direct I/O while the server runs.  The ublk block device is
 * made live as /dev/ublkb<N>.  Call unvmed_ublk_server_stop() to tear it down.
 *
 * @cpus: optional CPU list ("2,3,8-11") to pin the busy-polling queue
 * handler threads to; queue i runs on the (i % n)-th listed CPU.  NULL or
 * empty leaves the threads unpinned.
 *
 * Returns the server handle on success, NULL on error with errno set.
 */
struct unvmed_ublk_server *unvmed_ublk_server_start(struct unvme *u,
						    uint32_t nsid,
						    uint32_t queue_depth,
						    uint32_t poll_spin_us,
						    const char *cpus);

/*
 * Stop the server: signals handler threads, drains in-flight NVMe commands,
 * issues STOP_DEV / DEL_DEV, unmaps buffers, clears UNVMED_SQ_F_UBLK_OWNED
 * on all reused SQs, frees all resources.
 */
int unvmed_ublk_server_stop(struct unvmed_ublk_server *server);

/* Internal: per-queue thread entry point (called from unvmed-ublk.c) */
void *unvmed_ublk_queue_handler(void *arg);

/* Find the active ublk server for the given unvme controller, or NULL. */
struct unvmed_ublk_server *unvmed_ublk_find_server(struct unvme *u);

/*
 * Stop all active ublk servers.  Called from the SIGTERM handler before
 * NVMe controller teardown so that queue handler threads exit cleanly and
 * kernel ublk state is properly released, preventing D-state processes.
 */
void unvmed_ublk_stop_all_servers(void);


#endif /* UNVMED_UBLK_H */
