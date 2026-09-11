/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */
#ifndef LIBUNVMED_TRACE_H
#define LIBUNVMED_TRACE_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/time.h>

#include <vfn/nvme.h>
#include <nvme/types.h>

/*
 * On-disk format for the per-I/O trace.
 *
 * Why a second file rather than more lines in unvmed.log: rendering a log
 * line costs far more than capturing one, so at fio rates the writer cannot
 * keep up and records get dropped.  These records are written raw and
 * rendered only when someone reads them back — the same split blktrace makes
 * with blkparse.  /var/log/unvmed.log keeps holding ordinary text logs, so
 * cat and grep still work there.
 *
 * Everything here is plain data.  Records are read back by a *different
 * process* (`unvme log`), so a record may not contain pointers — not even to
 * string literals, whose addresses differ under ASLR.  The function name is
 * therefore derived from @type rather than stored.
 */

#define UNVME_TRACE_MAGIC	"UNVMTRC"	/* 7 chars + NUL */
#define UNVME_TRACE_VERSION	1u

struct unvme_trace_file_hdr {
	char     magic[8];
	uint32_t version;
	uint32_t hdr_size;	/* bytes to skip to reach the first record */
	uint64_t pid;
};

enum unvme_trace_type {
	UNVME_TRACE_CMD_POST = 1,
	UNVME_TRACE_CMD_CMPL,
	UNVME_TRACE_VCQ_PUSH,
	UNVME_TRACE_VCQ_POP,
};

#define UNVME_TRACE_BDF_LEN	16

/* @len counts the whole record, so a reader can skip a kind it does not know. */
struct unvme_trace_hdr {
	uint16_t len;
	uint8_t  type;
	uint8_t  rsvd;
	uint32_t line;
	int64_t  tv_sec;
	int32_t  tv_usec;
	uint32_t rsvd2;
};

struct unvme_trace_post {
	struct unvme_trace_hdr h;
	char                   bdf[UNVME_TRACE_BDF_LEN];
	uint32_t               sqid;
	uint32_t               rsvd;
	union nvme_cmd         sqe;
};

struct unvme_trace_cqe {
	struct unvme_trace_hdr h;
	char                   bdf[UNVME_TRACE_BDF_LEN];
	struct nvme_cqe        cqe;
};

/*
 * Render one record as the log line it would have been.  Returns the number
 * of bytes written (newline included), or 0 if the record is malformed or of
 * an unknown kind.
 */
uint32_t unvmed_trace_render(const void *rec, uint32_t len,
			     char *out, uint32_t outsz);

/*
 * Print the daemon's logs: the text log and the binary trace merged back into
 * one time-ordered stream, which is what `unvme log` shows.
 *
 * @nvme_only limits output to trace records.  @follow keeps waiting for new
 * records instead of returning at end of file; it returns only on error.
 * Either path may be NULL or missing, and is then simply skipped.
 */
int unvmed_log_dump(FILE *out, const char *text_path, const char *trace_path,
		    bool nvme_only, bool follow);

#endif /* LIBUNVMED_TRACE_H */
