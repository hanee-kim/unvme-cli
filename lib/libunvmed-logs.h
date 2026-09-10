/* SPDX-License-Identifier: LGPL-2.1-or-later OR MIT */

#ifndef LIBUNVMED_LOG_H
#define LIBUNVMED_LOG_H

#include <stdatomic.h>
#include "libunvmed-jump-label.h"

extern int __unvmed_logfd;
extern _Atomic int __log_level;

enum {
	UNVME_LOG_ERR,
	UNVME_LOG_INFO,
	UNVME_LOG_DEBUG,
	UNVME_LOG_LAST = UNVME_LOG_DEBUG,
};

#define loglv_to_str(lv) (lv == UNVME_LOG_ERR ? "ERROR" : \
		    lv == UNVME_LOG_INFO ? "INFO" : "DEBUG")

/*
 * Per-level static keys.  Defined in libunvmed-logs.c.
 *
 *   unvmed_log_key_info  — controls INFO-level output
 *   unvmed_log_key_debug — controls DEBUG-level output
 *
 * ERR is always written; no key is needed.
 */
DECLARE_STATIC_KEY_FALSE(unvmed_log_key_info);
DECLARE_STATIC_KEY_FALSE(unvmed_log_key_debug);

/*
 * Cold write path — noinline + cold keeps the timestamp/ring-push code out
 * of the hot I/O path's instruction cache.
 */
__attribute__((cold, noinline, format(printf, 4, 5)))
void __unvmed_log_write(int lv, const char *func, int line,
			const char *fmt, ...);

/*
 * unvmed_log_err — always written, no branch at all.
 */
#define unvmed_log_err(fmt, ...)					\
	__unvmed_log_write(UNVME_LOG_ERR, __func__, __LINE__,		\
			   fmt, ##__VA_ARGS__)

/*
 * unvmed_log_info — Linux kernel jump-label style:
 *   key disabled → 5-byte NOP, zero overhead
 *   key enabled  → 5-byte JMP, always-taken, no misprediction
 */
#define unvmed_log_info(fmt, ...)					\
	do {								\
		if (unvmed_static_branch_unlikely(&unvmed_log_key_info))\
			__unvmed_log_write(UNVME_LOG_INFO, __func__,	\
					   __LINE__, fmt, ##__VA_ARGS__);\
	} while (0)

/*
 * unvmed_log_debug:
 *   Release build (UNVME_DEBUG undefined):
 *     Expands to dead code — compiler eliminates it entirely.
 *     Zero instructions in the binary.  The if(0) wrapper still
 *     type-checks format arguments at compile time (Linux pr_debug style).
 *
 *   Debug build (UNVME_DEBUG defined):
 *     Same jump-label approach as unvmed_log_info.
 */
#ifdef UNVME_DEBUG
# define unvmed_log_debug(fmt, ...)					\
	do {								\
		if (unvmed_static_branch_unlikely(&unvmed_log_key_debug))\
			__unvmed_log_write(UNVME_LOG_DEBUG, __func__,	\
					   __LINE__, fmt, ##__VA_ARGS__);\
	} while (0)
#else
# define unvmed_log_debug(fmt, ...)					\
	do { if (0) __unvmed_log_write(UNVME_LOG_DEBUG, __func__,	\
				       __LINE__, fmt, ##__VA_ARGS__);	\
	} while (0)
#endif

/*
 * unvmed_log_set_level — update both __log_level and the static keys.
 * Call this instead of writing __log_level directly.
 */
void unvmed_log_set_level(int level);

/*
 * libunvmed-logs.c
 */
void unvmed_log_cmd_post(const char *bdf, uint32_t sqid, union nvme_cmd *sqe);
void unvmed_log_cmd_cmpl(const char *bdf, struct nvme_cqe *cqe);
void unvmed_log_cmd_vcq_push(struct nvme_cqe *cqe);
void unvmed_log_cmd_vcq_pop(struct nvme_cqe *cqe);

#endif
