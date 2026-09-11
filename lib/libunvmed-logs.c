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
#include "libunvmed-trace.h"
#include "libunvmed-private.h"

/* Static keys — one per log level that can be toggled at runtime.
 * Initially disabled (all NOPs at branch sites). */
DEFINE_STATIC_KEY_FALSE(unvmed_log_key_info);
DEFINE_STATIC_KEY_FALSE(unvmed_log_key_debug);

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
 * Capture helpers for the per-I/O trace records.
 *
 * These run on the I/O thread, so they only copy raw fields; the text is
 * produced later by whoever reads the trace back (libunvmed-trace.c).
 *
 * What may be stored is constrained by the reader being a *different
 * process*: no pointers survive, so the bdf is copied and the capture site
 * is identified by the record kind rather than by a __func__ pointer.
 */
static inline void unvmed_trace_hdr_init(struct unvme_trace_hdr *h,
					 uint8_t type, uint16_t len,
					 uint32_t line)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	h->len     = len;
	h->type    = type;
	h->rsvd    = 0;
	h->line    = line;
	h->tv_sec  = (int64_t)tv.tv_sec;
	h->tv_usec = (int32_t)tv.tv_usec;
	h->rsvd2   = 0;
}

/* Copy at most UNVME_TRACE_BDF_LEN-1 bytes and always NUL-terminate. */
static inline void unvmed_trace_set_bdf(char *dst, const char *bdf)
{
	if (!bdf) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, bdf, UNVME_TRACE_BDF_LEN - 1);
	dst[UNVME_TRACE_BDF_LEN - 1] = '\0';
}

/*
 * ---------------------------------------------------------------------------
 * Capture side — runs on the I/O thread.  Copies raw fields, formats nothing.
 * ---------------------------------------------------------------------------
 *
 * The #ifdef (not #ifndef) matters: these records only exist when UNVME_DEBUG
 * is set, so the two build modes need opposite things.  The inverted test got
 * both wrong — a release build captured every command and emitted nothing,
 * and a debug build captured every command even at ERROR level because the
 * guard had been preprocessed away.
 */
void unvmed_log_cmd_post(const char *bdf, uint32_t sqid, union nvme_cmd *sqe)
{
#ifndef UNVME_DEBUG
	(void)bdf;
	(void)sqid;
	(void)sqe;
#else
	struct unvme_trace_post rec;

	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;

	unvmed_trace_hdr_init(&rec.h, UNVME_TRACE_CMD_POST, sizeof(rec),
			      __LINE__);
	unvmed_trace_set_bdf(rec.bdf, bdf);
	rec.sqid = sqid;
	rec.rsvd = 0;
	rec.sqe  = *sqe;

	unvmed_log_ring_push(&__log_ring, UNVME_TRACE_CMD_POST,
			     &rec, sizeof(rec));
#endif /* UNVME_DEBUG */
}

void unvmed_log_cmd_cmpl(const char *bdf, struct nvme_cqe *cqe)
{
#ifndef UNVME_DEBUG
	(void)bdf;
	(void)cqe;
#else
	struct unvme_trace_cqe rec;

	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;

	unvmed_trace_hdr_init(&rec.h, UNVME_TRACE_CMD_CMPL, sizeof(rec),
			      __LINE__);
	unvmed_trace_set_bdf(rec.bdf, bdf);
	rec.cqe = *cqe;

	unvmed_log_ring_push(&__log_ring, UNVME_TRACE_CMD_CMPL,
			     &rec, sizeof(rec));
#endif /* UNVME_DEBUG */
}

static inline void unvmed_log_vcq(struct nvme_cqe *cqe, uint8_t type,
				  uint32_t line)
{
	struct unvme_trace_cqe rec;

	unvmed_trace_hdr_init(&rec.h, type, sizeof(rec), line);
	rec.bdf[0] = '\0';
	rec.cqe = *cqe;

	unvmed_log_ring_push(&__log_ring, type, &rec, sizeof(rec));
}

void unvmed_log_cmd_vcq_push(struct nvme_cqe *cqe)
{
#ifndef UNVME_DEBUG
	(void)cqe;
#else
	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;
	unvmed_log_vcq(cqe, UNVME_TRACE_VCQ_PUSH, __LINE__);
#endif
}

void unvmed_log_cmd_vcq_pop(struct nvme_cqe *cqe)
{
#ifndef UNVME_DEBUG
	(void)cqe;
#else
	if (!unvmed_static_branch_unlikely(&unvmed_log_key_debug))
		return;
	unvmed_log_vcq(cqe, UNVME_TRACE_VCQ_POP, __LINE__);
#endif
}
