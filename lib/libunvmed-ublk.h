/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_UBLK_H
#define LIBUNVMED_UBLK_H

#include "libunvmed.h"

/*
 * struct unvme_ublk_dev - opaque handle for a running ublk server instance.
 *
 * Created by unvmed_ublk_start(), destroyed by unvmed_ublk_stop().
 * Internally tracks the ublk character/block device pair plus all per-queue
 * worker threads.
 */
struct unvme_ublk_dev;

/**
 * unvmed_ublk_start - Create a ublk block device backed by an NVMe namespace.
 * @u:            libunvmed controller instance (already enabled, queues set up)
 * @nsid:         NVMe namespace ID to forward I/O to
 * @dev_id:       ublk device number — produces /dev/ublkb{dev_id}
 * @nr_queues:    number of ublk/NVMe queue pairs (= number of worker threads)
 * @queue_depth:  maximum in-flight I/Os per queue
 * @base_sqid:    first NVMe I/O queue ID to allocate; queues base_sqid …
 *                base_sqid+nr_queues-1 (and matching CQ IDs) are created
 *
 * Spawns @nr_queues background threads.  Each thread owns one ublk hardware
 * queue and one NVMe SQ/CQ pair.  The function returns as soon as
 * /dev/ublkb{dev_id} is live and ready for I/O.
 *
 * Returns a handle on success, NULL on failure.
 */
struct unvme_ublk_dev *unvmed_ublk_start(struct unvme *u, uint32_t nsid,
					  int dev_id, int nr_queues,
					  int queue_depth, int base_sqid);

/**
 * unvmed_ublk_stop - Stop the ublk server and release all resources.
 * @dev:  handle returned by unvmed_ublk_start()
 *
 * Signals every worker thread to stop, waits for them to drain in-flight I/O,
 * destroys the ublk device (/dev/ublkb{dev_id} disappears), deletes the NVMe
 * queues, and frees all memory.
 */
void unvmed_ublk_stop(struct unvme_ublk_dev *dev);

#endif /* LIBUNVMED_UBLK_H */
