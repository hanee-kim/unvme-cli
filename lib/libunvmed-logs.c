// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <vfn/nvme.h>
#include <nvme/types.h>
#include <ccan/list/list.h>

#include "libunvmed.h"
#include "libunvmed-logs.h"
#include "libunvmed-log-ring.h"
#include "libunvmed-private.h"

/* Static keys — one per log level that can be toggled at runtime.
 * Initially disabled (all NOPs at branch sites). */
DEFINE_STATIC_KEY_FALSE(unvmed_log_key_info);
DEFINE_STATIC_KEY_FALSE(unvmed_log_key_debug);

static uint32_t unvmed_log_format(uint8_t type, const void *rec, uint32_t len,
				  char *out, uint32_t outsz);

/*
 * Hands unvmed_init() the renderer for binary records without exporting the
 * record layouts, which stay private to this file.
 */
unvmed_log_format_fn unvmed_log_formatter(void)
{
	return unvmed_log_format;
}

/*
 * unvmed_log_set_level — single call-site for changing the log level.
 * Updates __log_level for display and patches the branch sites for the
 * INFO and DEBUG static keys.
 */
void unvmed_log_set_level(int level)
{
	/* release, not relaxed: pairs with readers so a thread that observes
	 * the newly patched static key cannot still read a stale level. */
	atomic_store_explicit(&__log_level, level, memory_order_release);

	if (level >= UNVME_LOG_INFO)
		unvmed_static_key_enable(&unvmed_log_key_info);
	else
		unvmed_static_key_disable(&unvmed_log_key_info);

	if (level >= UNVME_LOG_DEBUG)
		unvmed_static_key_enable(&unvmed_log_key_debug);
	else
		unvmed_static_key_disable(&unvmed_log_key_debug);
}

/*
 * Seconds-resolution timestamp cache.
 *
 * localtime_r() + strftime() together cost more than everything else in a log
 * line, and their result only changes once a second.  Cache the formatted
 * "YYYY-MM-DD HH:MM:SS" and rebuild it only when the second rolls over.
 *
 * Per-thread, so there is no lock and no cross-core sharing; each logging
 * thread keeps its own copy hot in its own cache.
 */
static __thread time_t __dt_sec = (time_t)-1;
static __thread char   __dt_date[20];	/* "YYYY-MM-DD HH:MM:SS" + NUL */
static __thread size_t __dt_len;

/* Six fixed digits, no snprintf: this runs on every single log line. */
static inline void unvme_usec6(char *p, long usec)
{
	for (int i = 5; i >= 0; i--) {
		p[i] = (char)('0' + (usec % 10));
		usec /= 10;
	}
}

static void unvme_datetime_tv(const struct timeval *tvp, char *datetime,
			      size_t sz)
{
	struct timeval tv = *tvp;

	if (tv.tv_sec != __dt_sec) {
		struct tm tmv;

		/* localtime_r is thread-safe (unlike localtime). */
		if (!localtime_r(&tv.tv_sec, &tmv)) {
			/* Fallback: use epoch seconds on localtime_r failure. */
			snprintf(datetime, sz, "%ld.??\?\?\?\?", (long)tv.tv_sec);
			return;
		}

		__dt_len = strftime(__dt_date, sizeof(__dt_date),
				    "%Y-%m-%d %H:%M:%S", &tmv);
		if (!__dt_len) {
			/* strftime buffer too small or locale issue. */
			snprintf(datetime, sz, "\?\?\?\?-\?\?-\?\? \?\?:\?\?:\?\?");
			return;
		}

		__dt_sec = tv.tv_sec;
	}

	/* date + '.' + 6 digits + NUL */
	if (sz < __dt_len + 8) {
		snprintf(datetime, sz, "%ld", (long)tv.tv_sec);
		return;
	}

	memcpy(datetime, __dt_date, __dt_len);
	datetime[__dt_len] = '.';
	unvme_usec6(datetime + __dt_len + 1, (long)tv.tv_usec);
	datetime[__dt_len + 7] = '\0';
}

static void unvme_datetime(char *datetime, size_t sz)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	unvme_datetime_tv(&tv, datetime, sz);
}

/*
 * Cold write path — formats the log line into a stack buffer and enqueues
 * it on the ring buffer.  The logger thread handles the actual file write,
 * so this function never issues a syscall and never blocks.
 */
__attribute__((cold, noinline, format(printf, 4, 5)))
void __unvmed_log_write(int lv, const char *func, int line,
			const char *fmt, ...)
{
	static const char * const lvstr[] = {
		[UNVME_LOG_ERR]   = "ERROR   ",
		[UNVME_LOG_INFO]  = "INFO    ",
		[UNVME_LOG_DEBUG] = "DEBUG   ",
	};
	char     datetime[32];
	char     msg[UNVMED_LOG_MSG_SIZE];
	va_list  va;
	int      n, m;

	/* Bounds-check lv to avoid OOB access on lvstr. */
	if ((unsigned)lv > UNVME_LOG_LAST)
		lv = UNVME_LOG_ERR;

	unvme_datetime(datetime, sizeof(datetime));

	n = snprintf(msg, sizeof(msg), "%s| %s | %s: %d: ",
		     lvstr[lv], datetime, func, line);
	/* Guard against snprintf truncation before appending the body. */
	if (n < 0)
		n = 0;
	if (n >= (int)sizeof(msg) - 1)
		n = (int)sizeof(msg) - 2;

	va_start(va, fmt);
	m = vsnprintf(msg + n, sizeof(msg) - (size_t)n, fmt, va);
	va_end(va);

	if (m > 0)
		n += (m < (int)(sizeof(msg) - (size_t)n)) ? m
						: (int)(sizeof(msg) - (size_t)n) - 1;

	/* Append newline, ensuring it always fits. */
	if (n < (int)sizeof(msg) - 1) {
		msg[n++] = '\n';
	} else {
		n = (int)sizeof(msg) - 1;
		msg[n - 1] = '\n';
	}

	unvmed_log_ring_push(&__log_ring, UNVMED_LOG_REC_TEXT, msg, (uint32_t)n);
}

/*
 * Binary records for the per-I/O log sites.
 *
 * unvmed_log_cmd_post/cmpl and the vCQ pair run once per submitted command
 * and once per completion, so rendering them on the calling thread puts a
 * vsnprintf — the single most expensive thing in a log line — directly in the
 * I/O path.  Instead the caller stores the raw fields and the logger thread
 * renders them, which is the same division of labour as a kernel tracepoint:
 * writing the event is cheap and bounded, and the expensive text only exists
 * if and when someone reads the trace.
 *
 * What may be stored:
 *   - @func comes from __func__, which has static storage duration, so the
 *     pointer stays valid for the life of the process.
 *   - @bdf is *copied*, not pointed to: it lives in the controller and the
 *     controller can be freed between the push and the render.
 *   - the SQE/CQE are copied by value; they are small and the caller's copy
 *     is reused as soon as we return.
 */
enum {
	UNVMED_LOG_REC_CMD_POST = 1,
	UNVMED_LOG_REC_CMD_CMPL,
	UNVMED_LOG_REC_VCQ_PUSH,
	UNVMED_LOG_REC_VCQ_POP,
};

#define UNVMED_LOG_BDF_LEN	16

struct unvmed_log_rec_hdr {
	struct timeval  tv;
	const char     *func;
	uint32_t        line;
};

struct unvmed_log_rec_post {
	struct unvmed_log_rec_hdr hdr;
	char            bdf[UNVMED_LOG_BDF_LEN];
	uint32_t        sqid;
	union nvme_cmd  sqe;
};

struct unvmed_log_rec_cqe {
	struct unvmed_log_rec_hdr hdr;
	char            bdf[UNVMED_LOG_BDF_LEN];
	struct nvme_cqe cqe;
};

static inline void unvmed_log_rec_hdr_init(struct unvmed_log_rec_hdr *h,
					   const char *func, uint32_t line)
{
	gettimeofday(&h->tv, NULL);
	h->func = func;
	h->line = line;
}

/* Copy at most UNVMED_LOG_BDF_LEN-1 bytes and always NUL-terminate. */
static inline void unvmed_log_rec_set_bdf(char *dst, const char *bdf)
{
	if (!bdf) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, bdf, UNVMED_LOG_BDF_LEN - 1);
	dst[UNVMED_LOG_BDF_LEN - 1] = '\0';
}

/* Per-thread scratch buffer for command-description helpers.
 * 'static' prevents it from being exported from the DSO. */
static __thread char __buf[256];
#define LOG_MAX_LEN sizeof(__buf)

static const char *unvmed_log_delete_sq(union nvme_cmd *sqe)
{
	uint16_t sqid = le32_to_cpu(sqe->cdw10) & 0xFFFF;

	snprintf(__buf, LOG_MAX_LEN, "unvmed_admin_delete_sq sqid=%u", sqid);

	return __buf;
}

static const char *unvmed_log_create_sq(union nvme_cmd *sqe)
{
	uint16_t sqid = le32_to_cpu(sqe->cdw10) & 0xFFFF;
	uint16_t qsize = (le32_to_cpu(sqe->cdw10) >> 16) & 0xFFFF;
	uint16_t sq_flags = le32_to_cpu(sqe->cdw11) & 0xFFFF;
	uint16_t cqid = (le32_to_cpu(sqe->cdw11) >> 16) & 0xFFFF;

	snprintf(__buf, LOG_MAX_LEN, "unvmed_admin_create_sq "
		 "sqid=%u, qsize=%u, sq_flags=0x%x, cqid=%u",
		 sqid, qsize, sq_flags, cqid);

	return __buf;
}

static const char *unvmed_log_delete_cq(union nvme_cmd *sqe)
{
	uint16_t cqid = le32_to_cpu(sqe->cdw10) & 0xFFFF;

	snprintf(__buf, LOG_MAX_LEN, "unvmed_admin_delete_cq cqid=%u", cqid);

	return __buf;
}

static const char *unvmed_log_create_cq(union nvme_cmd *sqe)
{
	uint16_t cqid = le32_to_cpu(sqe->cdw10) & 0xFFFF;
	uint16_t qsize = (le32_to_cpu(sqe->cdw10) >> 16) & 0xFFFF;
	uint16_t cq_flags = le32_to_cpu(sqe->cdw11) & 0xFFFF;
	uint16_t irq_vector = (le32_to_cpu(sqe->cdw11) >> 16) & 0xFFFF;

	snprintf(__buf, LOG_MAX_LEN, "unvmed_admin_create_cq "
		 "cqid=%u, qsize=%u, cq_flags=0x%x, irq_vector=%u",
		 cqid, qsize, cq_flags, irq_vector);

	return __buf;
}

static const char *unvmed_log_admin_identify(union nvme_cmd *sqe)
{
	uint8_t cns = le32_to_cpu(sqe->cdw10) & 0xFF;
	uint16_t ctrlid = (le32_to_cpu(sqe->cdw10) >> 16) & 0xFFFF;

	snprintf(__buf, LOG_MAX_LEN, "nvme_admin_identify "
		 "cns=%u, ctrlid=%u", cns, ctrlid);

	return __buf;
}

static const char *unvmed_log_read_write(union nvme_cmd *sqe)
{
	uint64_t slba = ((uint64_t) le32_to_cpu(sqe->cdw11) << 32) |
		le32_to_cpu(sqe->cdw10);
	uint16_t length = le32_to_cpu(sqe->cdw12) & 0xFFFF;
	uint16_t control = (le32_to_cpu(sqe->cdw12) >> 16) & 0xFFFF;
	uint32_t dsmgmt = le32_to_cpu(sqe->cdw13);
	uint32_t reftag = le32_to_cpu(sqe->cdw14);

	if (sqe->opcode == 1)
		snprintf(__buf, LOG_MAX_LEN, "write "
			 "slba=%lu, len=%u, ctrl=0x%x, dsmgmt=%u, reftag=%u",
			 slba, length, control, dsmgmt, reftag);
	else
		snprintf(__buf, LOG_MAX_LEN, "read "
			 "slba=%lu, len=%u, ctrl=0x%x, dsmgmt=%u, reftag=%u",
			 slba, length, control, dsmgmt, reftag);

	return __buf;
}

static const char *unvmed_log_common(union nvme_cmd *sqe)
{
	snprintf(__buf, LOG_MAX_LEN, "opcode=0x%x, cdw2=0x%x, cdw3=0x%x, "
		 "cdw10=0x%x, cdw11=0x%x, cdw12=0x%x, "
		 "cdw13=0x%x, cdw14=0x%x, cdw15=0x%x",
		 sqe->opcode, le32_to_cpu(sqe->cdw2), le32_to_cpu(sqe->cdw3),
		 le32_to_cpu(sqe->cdw10), le32_to_cpu(sqe->cdw11),
		 le32_to_cpu(sqe->cdw12), le32_to_cpu(sqe->cdw13),
		 le32_to_cpu(sqe->cdw14), le32_to_cpu(sqe->cdw15));

	return __buf;
}

static const char *unvmed_log_io_cmd(union nvme_cmd *sqe)
{
	switch (sqe->opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
		return unvmed_log_read_write(sqe);
	}

	return unvmed_log_common(sqe);
}

static const char *unvmed_log_admin_cmd(union nvme_cmd *sqe)
{
	switch (sqe->opcode) {
	case nvme_admin_delete_sq:
		return unvmed_log_delete_sq(sqe);
	case nvme_admin_create_sq:
		return unvmed_log_create_sq(sqe);
	case nvme_admin_delete_cq:
		return unvmed_log_delete_cq(sqe);
	case nvme_admin_create_cq:
		return unvmed_log_create_cq(sqe);
	case nvme_admin_identify:
		return unvmed_log_admin_identify(sqe);
	}

	return unvmed_log_common(sqe);
}

/*
 * ---------------------------------------------------------------------------
 * Render side — runs on the logger thread, never on an I/O thread.
 * ---------------------------------------------------------------------------
 */

/* Common "LEVEL | datetime | func: line: " prefix, from the record's own
 * timestamp so lines carry the time of the event, not of the rendering. */
static uint32_t unvmed_log_rec_prefix(const struct unvmed_log_rec_hdr *h,
				      char *out, uint32_t outsz)
{
	char datetime[32];
	int  n;

	unvme_datetime_tv(&h->tv, datetime, sizeof(datetime));
	n = snprintf(out, outsz, "DEBUG   | %s | %s: %u: ",
		     datetime, h->func ? h->func : "?", h->line);

	if (n < 0)
		return 0;
	return (n >= (int)outsz) ? outsz - 1 : (uint32_t)n;
}

static uint32_t unvmed_log_render_cqe(const char *tag, const char *bdf,
				      const struct nvme_cqe *cqe,
				      char *out, uint32_t outsz)
{
	uint16_t sfp = le16_to_cpu(cqe->sfp);
	int n;

	if (bdf && bdf[0])
		n = snprintf(out, outsz, "%s: %s: cqe (qid=%d, cid=%d, ",
			     tag, bdf, cqe->sqid, cqe->cid);
	else
		n = snprintf(out, outsz, "cqe (qid=%d, cid=%d, ",
			     cqe->sqid, cqe->cid);
	if (n < 0 || (uint32_t)n >= outsz)
		return 0;

	int m = snprintf(out + n, outsz - (uint32_t)n,
			 "dw0=0x%x, dw1=0x%x, head=%d, phase=%d, sct=0x%x, "
			 "sc=0x%x, crd=0x%x, more=%d, dnr=%d)\n",
			 le32_to_cpu(cqe->dw0), le32_to_cpu(cqe->dw1),
			 le16_to_cpu(cqe->sqhd), sfp & 0x1,
			 (sfp >> 9) & 0x7, (sfp >> 1) & 0xFF,
			 (sfp >> 12) & 0xFF, (sfp >> 14) & 0x1,
			 (sfp >> 15) & 0x1);
	if (m < 0)
		return 0;
	return (uint32_t)n + (((uint32_t)m >= outsz - (uint32_t)n)
			      ? outsz - (uint32_t)n - 1 : (uint32_t)m);
}

static uint32_t unvmed_log_render_post(const struct unvmed_log_rec_post *p,
				       char *out, uint32_t outsz)
{
	const union nvme_cmd *sqe = &p->sqe;
	bool admin = !p->sqid;
	int psdt = (sqe->flags >> 6) & 0x3;
	const char *psdt_type;
	uint64_t dptr0 = 0, dptr1 = 0;
	const char *str;
	int n;

	switch (psdt) {
	case 0:
		psdt_type = "prp";
		dptr0 = le64_to_cpu(sqe->dptr.prp1);
		dptr1 = le64_to_cpu(sqe->dptr.prp2);
		break;
	case 1:
	case 2:
		psdt_type = "sgl";
		dptr0 = le64_to_cpu(sqe->dptr.sgl.addr);
		dptr1 = le32_to_cpu(sqe->dptr.sgl.len);
		break;
	default:
		psdt_type = "reserved";
	}

	str = admin ? unvmed_log_admin_cmd((union nvme_cmd *)sqe)
		    : unvmed_log_io_cmd((union nvme_cmd *)sqe);

	n = snprintf(out, outsz,
		     "unvmed_cmd_post: %s: sqe (qid=%d, cid=%d, nsid=%d, "
		     "fuse=0x%x, psdt=%d(%s), mptr=0x%lx, "
		     "dptr0=0x%lx, dptr1=0x%lx, cmd=(%s))\n",
		     p->bdf, p->sqid, sqe->cid, le32_to_cpu(sqe->nsid),
		     sqe->flags & 0x3, psdt, psdt_type,
		     le64_to_cpu(sqe->mptr), dptr0, dptr1, str);
	if (n < 0)
		return 0;
	return ((uint32_t)n >= outsz) ? outsz - 1 : (uint32_t)n;
}

/*
 * Ring formatter callback.  Returns bytes written; 0 drops the record.
 * Every path leaves the line newline-terminated.
 */
static uint32_t unvmed_log_format(uint8_t type, const void *rec, uint32_t len,
				  char *out, uint32_t outsz)
{
	const struct unvmed_log_rec_hdr *hdr = rec;
	uint32_t n;

	if (len < sizeof(*hdr) || outsz < 64)
		return 0;

	n = unvmed_log_rec_prefix(hdr, out, outsz);
	if (!n)
		return 0;

	switch (type) {
	case UNVMED_LOG_REC_CMD_POST:
		if (len < sizeof(struct unvmed_log_rec_post))
			return 0;
		n += unvmed_log_render_post(rec, out + n, outsz - n);
		break;
	case UNVMED_LOG_REC_CMD_CMPL:
	case UNVMED_LOG_REC_VCQ_PUSH:
	case UNVMED_LOG_REC_VCQ_POP: {
		const struct unvmed_log_rec_cqe *c = rec;

		if (len < sizeof(*c))
			return 0;
		n += unvmed_log_render_cqe(
			type == UNVMED_LOG_REC_CMD_CMPL ? "unvmed_cmd_cmpl"
							: NULL,
			c->bdf, &c->cqe, out + n, outsz - n);
		break;
	}
	default:
		return 0;
	}

	/* Guarantee termination even if a renderer truncated. */
	if (out[n - 1] != '\n') {
		if (n >= outsz)
			n = outsz - 1;
		out[n++] = '\n';
	}
	return n;
}

/*
 * ---------------------------------------------------------------------------
 * Capture side — runs on the I/O thread.  No formatting happens here.
 * ---------------------------------------------------------------------------
 *
 * The #ifdef (not #ifndef) matters: unvmed_log_debug() and these records only
 * exist when UNVME_DEBUG is set, so the two build modes need opposite things.
 * The inverted test got both wrong — a release build captured every command
 * and emitted nothing, and a debug build captured every command even at ERROR
 * level because the guard had been preprocessed away.
 */
void unvmed_log_cmd_post(const char *bdf, uint32_t sqid, union nvme_cmd *sqe)
{
#ifndef UNVME_DEBUG
	(void)bdf;
	(void)sqid;
	(void)sqe;
#else
	struct unvmed_log_rec_post rec;

	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;

	unvmed_log_rec_hdr_init(&rec.hdr, __func__, __LINE__);
	unvmed_log_rec_set_bdf(rec.bdf, bdf);
	rec.sqid = sqid;
	rec.sqe  = *sqe;

	unvmed_log_ring_push(&__log_ring, UNVMED_LOG_REC_CMD_POST,
			     &rec, sizeof(rec));
#endif /* UNVME_DEBUG */
}

void unvmed_log_cmd_cmpl(const char *bdf, struct nvme_cqe *cqe)
{
#ifndef UNVME_DEBUG
	(void)bdf;
	(void)cqe;
#else
	struct unvmed_log_rec_cqe rec;

	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;

	unvmed_log_rec_hdr_init(&rec.hdr, __func__, __LINE__);
	unvmed_log_rec_set_bdf(rec.bdf, bdf);
	rec.cqe = *cqe;

	unvmed_log_ring_push(&__log_ring, UNVMED_LOG_REC_CMD_CMPL,
			     &rec, sizeof(rec));
#endif /* UNVME_DEBUG */
}

void unvmed_log_cmd_vcq_push(struct nvme_cqe *cqe)
{
#ifndef UNVME_DEBUG
	(void)cqe;
#else
	struct unvmed_log_rec_cqe rec;

	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;

	unvmed_log_rec_hdr_init(&rec.hdr, __func__, __LINE__);
	rec.bdf[0] = '\0';
	rec.cqe = *cqe;

	unvmed_log_ring_push(&__log_ring, UNVMED_LOG_REC_VCQ_PUSH,
			     &rec, sizeof(rec));
#endif /* UNVME_DEBUG */
}

void unvmed_log_cmd_vcq_pop(struct nvme_cqe *cqe)
{
#ifndef UNVME_DEBUG
	(void)cqe;
#else
	struct unvmed_log_rec_cqe rec;

	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;

	unvmed_log_rec_hdr_init(&rec.hdr, __func__, __LINE__);
	rec.bdf[0] = '\0';
	rec.cqe = *cqe;

	unvmed_log_ring_push(&__log_ring, UNVMED_LOG_REC_VCQ_POP,
			     &rec, sizeof(rec));
#endif /* UNVME_DEBUG */
}
