/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNVME_UBLK_H
#define UNVME_UBLK_H

#include <stdint.h>
#include "libunvmed.h"

/*
 * Start a ublk server that exposes /dev/ublkb<dev_id> backed by the given
 * NVMe namespace.  The server runs as background threads inside the daemon.
 *
 * @u:              libunvmed controller handle
 * @nsid:           NVMe namespace ID to back the block device
 * @nr_sectors:     total LBA count of the namespace
 * @lba_shift:      log2 of LBA size in bytes (9 = 512B, 12 = 4096B)
 * @dev_id:         ublk device ID (creates /dev/ublkb<dev_id>)
 * @nr_queues:      number of ublk queues (each mapped to one NVMe SQ/CQ pair)
 * @queue_depth:    depth of each queue
 * @start_sqid:     first NVMe I/O SQ ID to use (SQs start_sqid..start_sqid+nr_queues-1)
 * @max_io_kb:      maximum I/O size in KiB (capped by controller MDTS)
 *
 * Returns 0 on success, negative errno on failure.
 */
int unvme_ublk_server_start(struct unvme *u, uint32_t nsid,
			    uint64_t nr_sectors, uint8_t lba_shift,
			    int dev_id, int nr_queues, uint32_t queue_depth,
			    int start_sqid, uint32_t max_io_kb);

/*
 * Stop and tear down the ublk server for the given device ID.
 * Blocks until all threads have exited.
 */
int unvme_ublk_server_stop(int dev_id);

#endif /* UNVME_UBLK_H */
