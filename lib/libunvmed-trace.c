// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#define _GNU_SOURCE		/* strptime */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vfn/nvme.h>
#include <nvme/types.h>

#include "libunvmed-trace.h"

/*
 * ---------------------------------------------------------------------------
 * Rendering — runs in whoever reads the trace back, never in the I/O path.
 * ---------------------------------------------------------------------------
 */

/* The capture site is identified by the record kind, so the function name is
 * derived here instead of being stored in (and read back out of) the file. */
static const char *trace_func(uint8_t type)
{
	switch (type) {
	case UNVME_TRACE_CMD_POST: return "unvmed_log_cmd_post";
	case UNVME_TRACE_CMD_CMPL: return "unvmed_log_cmd_cmpl";
	case UNVME_TRACE_VCQ_PUSH: return "unvmed_log_cmd_vcq_push";
	case UNVME_TRACE_VCQ_POP:  return "unvmed_log_cmd_vcq_pop";
	default:                   return "?";
	}
}

static void trace_datetime(const struct unvme_trace_hdr *h, char *out,
			   size_t sz)
{
	time_t    secs = (time_t)h->tv_sec;
	struct tm tmv;
	size_t    n;

	if (!localtime_r(&secs, &tmv)) {
		snprintf(out, sz, "%lld.%06d", (long long)h->tv_sec, h->tv_usec);
		return;
	}

	n = strftime(out, sz, "%Y-%m-%d %H:%M:%S", &tmv);
	if (!n) {
		snprintf(out, sz, "%lld.%06d", (long long)h->tv_sec, h->tv_usec);
		return;
	}

	snprintf(out + n, sz - n, ".%06d", h->tv_usec);
}

/* Per-call scratch for the decoded command description. */
static const char *render_sq_cmd(const union nvme_cmd *sqe, bool admin,
				 char *buf, size_t sz)
{
	if (admin) {
		switch (sqe->opcode) {
		case nvme_admin_delete_sq:
			snprintf(buf, sz, "unvmed_admin_delete_sq sqid=%u",
				 (unsigned)(le32_to_cpu(sqe->cdw10) & 0xFFFF));
			return buf;
		case nvme_admin_create_sq:
			snprintf(buf, sz, "unvmed_admin_create_sq sqid=%u, "
				 "qsize=%u, sq_flags=0x%x, cqid=%u",
				 (unsigned)(le32_to_cpu(sqe->cdw10) & 0xFFFF),
				 (unsigned)((le32_to_cpu(sqe->cdw10) >> 16) & 0xFFFF),
				 (unsigned)(le32_to_cpu(sqe->cdw11) & 0xFFFF),
				 (unsigned)((le32_to_cpu(sqe->cdw11) >> 16) & 0xFFFF));
			return buf;
		case nvme_admin_delete_cq:
			snprintf(buf, sz, "unvmed_admin_delete_cq cqid=%u",
				 (unsigned)(le32_to_cpu(sqe->cdw10) & 0xFFFF));
			return buf;
		case nvme_admin_create_cq:
			snprintf(buf, sz, "unvmed_admin_create_cq cqid=%u, "
				 "qsize=%u, cq_flags=0x%x, irq_vector=%u",
				 (unsigned)(le32_to_cpu(sqe->cdw10) & 0xFFFF),
				 (unsigned)((le32_to_cpu(sqe->cdw10) >> 16) & 0xFFFF),
				 (unsigned)(le32_to_cpu(sqe->cdw11) & 0xFFFF),
				 (unsigned)((le32_to_cpu(sqe->cdw11) >> 16) & 0xFFFF));
			return buf;
		case nvme_admin_identify:
			snprintf(buf, sz, "nvme_admin_identify cns=%u, ctrlid=%u",
				 (unsigned)(le32_to_cpu(sqe->cdw10) & 0xFF),
				 (unsigned)((le32_to_cpu(sqe->cdw10) >> 16) & 0xFFFF));
			return buf;
		}
	} else if (sqe->opcode == nvme_cmd_write || sqe->opcode == nvme_cmd_read) {
		uint64_t slba = ((uint64_t)le32_to_cpu(sqe->cdw11) << 32) |
				le32_to_cpu(sqe->cdw10);

		snprintf(buf, sz, "%s slba=%lu, len=%u, ctrl=0x%x, "
			 "dsmgmt=%u, reftag=%u",
			 sqe->opcode == nvme_cmd_write ? "write" : "read",
			 (unsigned long)slba,
			 (unsigned)(le32_to_cpu(sqe->cdw12) & 0xFFFF),
			 (unsigned)((le32_to_cpu(sqe->cdw12) >> 16) & 0xFFFF),
			 le32_to_cpu(sqe->cdw13), le32_to_cpu(sqe->cdw14));
		return buf;
	}

	snprintf(buf, sz, "opcode=0x%x, cdw2=0x%x, cdw3=0x%x, cdw10=0x%x, "
		 "cdw11=0x%x, cdw12=0x%x, cdw13=0x%x, cdw14=0x%x, cdw15=0x%x",
		 sqe->opcode, le32_to_cpu(sqe->cdw2), le32_to_cpu(sqe->cdw3),
		 le32_to_cpu(sqe->cdw10), le32_to_cpu(sqe->cdw11),
		 le32_to_cpu(sqe->cdw12), le32_to_cpu(sqe->cdw13),
		 le32_to_cpu(sqe->cdw14), le32_to_cpu(sqe->cdw15));
	return buf;
}

uint32_t unvmed_trace_render(const void *rec, uint32_t len,
			     char *out, uint32_t outsz)
{
	const struct unvme_trace_hdr *h = rec;
	char datetime[40];
	char cmd[256];
	int  n;

	if (!rec || len < sizeof(*h) || outsz < 128)
		return 0;

	trace_datetime(h, datetime, sizeof(datetime));

	n = snprintf(out, outsz, "DEBUG   | %s | %s: %u: ",
		     datetime, trace_func(h->type), h->line);
	if (n < 0 || (uint32_t)n >= outsz)
		return 0;

	switch (h->type) {
	case UNVME_TRACE_CMD_POST: {
		const struct unvme_trace_post *p = rec;
		const union nvme_cmd *sqe = &p->sqe;
		const char *psdt_type;
		uint64_t dptr0 = 0, dptr1 = 0;
		int psdt;
		int m;

		if (len < sizeof(*p))
			return 0;

		psdt = (sqe->flags >> 6) & 0x3;
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

		m = snprintf(out + n, outsz - (uint32_t)n,
			     "unvmed_cmd_post: %s: sqe (qid=%d, cid=%d, "
			     "nsid=%d, fuse=0x%x, psdt=%d(%s), mptr=0x%lx, "
			     "dptr0=0x%lx, dptr1=0x%lx, cmd=(%s))\n",
			     p->bdf, p->sqid, sqe->cid,
			     le32_to_cpu(sqe->nsid), sqe->flags & 0x3,
			     psdt, psdt_type,
			     (unsigned long)le64_to_cpu(sqe->mptr),
			     (unsigned long)dptr0, (unsigned long)dptr1,
			     render_sq_cmd(sqe, !p->sqid, cmd, sizeof(cmd)));
		if (m < 0)
			return 0;
		n += ((uint32_t)m >= outsz - (uint32_t)n)
			? (int)(outsz - (uint32_t)n) - 1 : m;
		break;
	}
	case UNVME_TRACE_CMD_CMPL:
	case UNVME_TRACE_VCQ_PUSH:
	case UNVME_TRACE_VCQ_POP: {
		const struct unvme_trace_cqe *c = rec;
		const struct nvme_cqe *cqe;
		uint16_t sfp;
		int m;

		if (len < sizeof(*c))
			return 0;

		cqe = &c->cqe;
		sfp = le16_to_cpu(cqe->sfp);

		if (h->type == UNVME_TRACE_CMD_CMPL)
			m = snprintf(out + n, outsz - (uint32_t)n,
				     "unvmed_cmd_cmpl: %s: ", c->bdf);
		else
			m = 0;
		if (m < 0)
			return 0;
		n += m;
		if ((uint32_t)n >= outsz)
			return 0;

		m = snprintf(out + n, outsz - (uint32_t)n,
			     "cqe (qid=%d, cid=%d, dw0=0x%x, dw1=0x%x, "
			     "head=%d, phase=%d, sct=0x%x, sc=0x%x, crd=0x%x, "
			     "more=%d, dnr=%d)\n",
			     cqe->sqid, cqe->cid, le32_to_cpu(cqe->dw0),
			     le32_to_cpu(cqe->dw1), le16_to_cpu(cqe->sqhd),
			     sfp & 0x1, (sfp >> 9) & 0x7, (sfp >> 1) & 0xFF,
			     (sfp >> 12) & 0xFF, (sfp >> 14) & 0x1,
			     (sfp >> 15) & 0x1);
		if (m < 0)
			return 0;
		n += ((uint32_t)m >= outsz - (uint32_t)n)
			? (int)(outsz - (uint32_t)n) - 1 : m;
		break;
	}
	default:
		return 0;
	}

	if (out[n - 1] != '\n') {
		if ((uint32_t)n >= outsz)
			n = (int)outsz - 1;
		out[n++] = '\n';
	}
	return (uint32_t)n;
}

/*
 * ---------------------------------------------------------------------------
 * Merged playback — `unvme log`
 * ---------------------------------------------------------------------------
 *
 * Both sources are already in time order on their own, so this is a two-way
 * merge with no buffering of either whole file.
 */

struct text_src {
	FILE   *f;
	char   *line;
	size_t  cap;
	bool    valid;		/* a line is held and not yet printed */
	double  when;
};

struct trace_src {
	FILE    *f;
	char     rec[4096];
	uint32_t len;
	bool     valid;
	double   when;
};

/* "LEVEL   | YYYY-MM-DD HH:MM:SS.uuuuuu | ..." -> epoch seconds.
 * A line without a parsable stamp sorts as "now-ish"; it still prints, it
 * just cannot be ordered precisely against the trace. */
static double text_line_time(const char *line)
{
	const char *bar = strchr(line, '|');
	struct tm tmv;
	unsigned usec = 0;
	char datebuf[32];

	if (!bar)
		return -1.0;
	bar++;
	while (*bar == ' ')
		bar++;

	memset(&tmv, 0, sizeof(tmv));
	if (strlen(bar) < 19)
		return -1.0;
	memcpy(datebuf, bar, 19);
	datebuf[19] = '\0';

	if (!strptime(datebuf, "%Y-%m-%d %H:%M:%S", &tmv))
		return -1.0;
	tmv.tm_isdst = -1;

	if (bar[19] == '.')
		usec = (unsigned)strtoul(bar + 20, NULL, 10);

	return (double)mktime(&tmv) + (double)usec / 1e6;
}

static bool text_next(struct text_src *t)
{
	ssize_t n;

	if (!t->f || t->valid)
		return t->valid;

	n = getline(&t->line, &t->cap, t->f);
	if (n <= 0) {
		clearerr(t->f);
		return false;
	}

	t->when  = text_line_time(t->line);
	t->valid = true;
	return true;
}

/*
 * Read one record.  A short read means the writer is mid-record, so rewind to
 * where the record started and report "nothing yet" — the next call retries
 * and sees the whole thing.
 */
static bool trace_next(struct trace_src *s)
{
	struct unvme_trace_hdr h;
	long start;

	if (!s->f || s->valid)
		return s->valid;

	start = ftell(s->f);
	if (fread(&h, sizeof(h), 1, s->f) != 1) {
		clearerr(s->f);
		if (start >= 0)
			fseek(s->f, start, SEEK_SET);
		return false;
	}

	if (h.len < sizeof(h) || h.len > sizeof(s->rec)) {
		/* Corrupt length: there is no safe way to find the next record. */
		s->f = NULL;
		return false;
	}

	memcpy(s->rec, &h, sizeof(h));
	if (h.len > sizeof(h) &&
	    fread(s->rec + sizeof(h), h.len - sizeof(h), 1, s->f) != 1) {
		clearerr(s->f);
		if (start >= 0)
			fseek(s->f, start, SEEK_SET);
		return false;
	}

	s->len   = h.len;
	s->when  = (double)h.tv_sec + (double)h.tv_usec / 1e6;
	s->valid = true;
	return true;
}

static int trace_open(struct trace_src *s, const char *path)
{
	struct unvme_trace_file_hdr fh;

	memset(s, 0, sizeof(*s));
	if (!path)
		return 0;

	s->f = fopen(path, "r");
	if (!s->f)
		return 0;

	if (fread(&fh, sizeof(fh), 1, s->f) != 1 ||
	    memcmp(fh.magic, UNVME_TRACE_MAGIC, sizeof(UNVME_TRACE_MAGIC)) ||
	    fh.version != UNVME_TRACE_VERSION) {
		fclose(s->f);
		s->f = NULL;
		return -1;
	}

	/* Honour hdr_size so a newer writer's larger header is skipped. */
	if (fh.hdr_size > sizeof(fh))
		fseek(s->f, (long)fh.hdr_size, SEEK_SET);
	return 0;
}

int unvmed_log_dump(FILE *out, const char *text_path, const char *trace_path,
		    bool nvme_only, bool follow)
{
	struct text_src  t = { 0 };
	struct trace_src s;
	char line[UNVME_TRACE_BDF_LEN * 64];
	int  rc = 0;

	if (!nvme_only && text_path)
		t.f = fopen(text_path, "r");

	if (trace_open(&s, trace_path) < 0) {
		fprintf(out, "unvme: %s is not a valid trace file\n",
			trace_path);
		rc = -1;
	}

	if (!t.f && !s.f) {
		free(t.line);
		return rc ? rc : -1;
	}

	for (;;) {
		bool have_t = text_next(&t);
		bool have_s = trace_next(&s);

		if (!have_t && !have_s) {
			if (!follow)
				break;
			/* Nothing buffered anywhere: wait for the writer. */
			usleep(100000);
			continue;
		}

		/*
		 * Print whichever is older.  An unstamped text line (when < 0)
		 * is emitted immediately rather than held back forever.
		 */
		bool take_text;
		if (have_t && !have_s)
			take_text = true;
		else if (!have_t)
			take_text = false;
		else if (t.when < 0)
			take_text = true;
		else
			take_text = (t.when <= s.when);

		if (take_text) {
			fputs(t.line, out);
			t.valid = false;
		} else {
			uint32_t n = unvmed_trace_render(s.rec, s.len, line,
							 sizeof(line));
			if (n)
				fwrite(line, 1, n, out);
			s.valid = false;
		}
	}

	if (t.f)
		fclose(t.f);
	if (s.f)
		fclose(s.f);
	free(t.line);
	return rc;
}
