// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <sys/signal.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ccan/str/str.h>

#include "unvme.h"
#include "libunvmed-trace.h"

#include <argtable3.h>

static FILE *__stdout;
static FILE *__stderr;

static void __attribute__((constructor)) __unvme_cmd_init(void) {
	__stdout = stdout;
	__stderr = stderr;
}

#define unvme_parse_args(argc, argv, argtable, help, end, desc)				\
	do {										\
		int nerror = arg_parse(argc - 1, &argv[1], argtable);			\
											\
		if (arg_boolv(help)) {							\
			unvme_print_help(__stdout, argv[1], desc, argtable);		\
											\
			arg_freetable(argtable, sizeof(argtable) / sizeof(*argtable));	\
			return 0;							\
		}									\
											\
		if (nerror > 0) {							\
			arg_print_errors(__stdout, end, "unvme");                       \
			unvme_print_help(__stdout, argv[1], desc, argtable);		\
											\
			arg_freetable(argtable, sizeof(argtable) / sizeof(*argtable));	\
			return EINVAL;							\
		}									\
	} while (0)

static void __unvme_stop(bool with_client);

int unvme_start(int argc, char *argv[], struct unvme_msg *msg)
{
	pid_t pid;
	int ret = 0;
	struct arg_str *with_fio;
	struct arg_lit *restart;
	struct arg_lit *help;
	struct arg_end *end;
	const char *desc = "Start unvmed daemon process.";

	void *argtable[] = {
		with_fio = arg_str0(NULL, "with-fio", "</path/to/fio>", "[O] fio shared object path to run"),
		restart = arg_lit0("r", "restart", "Stop the current running unvmed and re-run the daemon process"),
		help = arg_lit0("h", "help", "Show help message"),
		end = arg_end(20),
	};

	arg_strv(with_fio) = NULL;

	unvme_parse_args(argc, argv, argtable, help, end, desc);

	if (unvme_is_daemon_running()) {
		if (arg_boolv(restart))
			__unvme_stop(true);
		else {
			unvme_pr_err("unvmed is already running\n");
			goto out;
		}
	}

	if (arg_boolv(with_fio) && access(arg_strv(with_fio), F_OK))
		arg_strv(with_fio) = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		ret = errno;
	} else if (!pid)
		ret = unvmed(argv, arg_strv(with_fio));
	else {
		while (!unvme_is_daemon_running())
			;
	}

out:
	unvme_free_args(argtable);
	return ret;
}

static int __kill(const char *name)
{
	char pids[16];
	char *cmd = NULL;
	pid_t pid;
	FILE *fp;
	int ret;

	ret = asprintf(&cmd, "pgrep %s", name);
	if (ret < 0)
		return ret;

	fp = popen(cmd, "r");
	if (!fp) {
		unvme_pr_err("failed to popen '%s'\n", cmd);
		free(cmd);
		return -errno;
	}

	free(cmd);

	/*
	 * Find all the unvme* processes running in the current system and
	 * terminate them gracefully.  If they are not terminated successfully,
	 * kill them all here.
	 */
	while (fgets(pids, sizeof(pids) - 1, fp) != NULL) {
		pid = atoi(pids);
		if (pid == getpid())
			continue;

		kill(pid, SIGTERM);

		for (int i = 0; i < 100; i++) {
			if (kill(pid, 0) == -1 && errno == ESRCH)
				break;
			usleep(10 * 1000);
		}

		if (!kill(pid, 0)) {
			bool killed = false;

			kill(pid, SIGKILL);
			for (int i = 0; i < 100; i++) {
				if (kill(pid, 0) == -1 && errno == ESRCH) {
					killed = true;
					break;
				}
				usleep(10 * 1000);
			}

			if (!killed)
				unvme_pr_err("failed to stop process(%d)\n", pid);
		}
	}

	pclose(fp);
	return 0;
}

static void __unvme_stop(bool with_client)
{
	__kill("unvmed");
	/* To make sure that unvmed.pid file is removed from the system */
	remove(UNVME_DAEMON_VER);
	remove(UNVME_DAEMON_PID);

	if (with_client)
		__kill("unvme");
}

int unvme_stop(int argc, char *argv[], struct unvme_msg *msg)
{
	struct arg_lit *all;
	struct arg_lit *help;
	struct arg_end *end;
	const char *desc = "Stop unvmed daemon process";

	void *argtable[] = {
		all = arg_lit0("a", "all", "Stop all unvme-cli clients along with unvmed"),
		help = arg_lit0("h", "help", "Show help message"),
		end = arg_end(20),
	};

	unvme_parse_args(argc, argv, argtable, help, end, desc);

	__unvme_stop(arg_boolv(all));

	unvme_free_args(argtable);
	return 0;
}

int unvme_log(int argc, char *argv[], struct unvme_msg *msg)
{
	struct arg_lit *nvme;
	struct arg_lit *follow;
	struct arg_lit *help;
	struct arg_end *end;
	const char *desc = "Show logs written by unvmed process.";

	void *argtable[] = {
		nvme = arg_lit0("n", "nvme", "Show NVMe command log only"),
		follow = arg_lit0("f", "follow", "Keep printing as new logs arrive"),
		help = arg_lit0("h", "help", "Show help message"),
		end = arg_end(20),
	};
	int ret;

	unvme_parse_args(argc, argv, argtable, help, end, desc);

	/*
	 * Two sources: the text log, and the binary trace holding the per-I/O
	 * records (which are captured raw so the I/O path never formats).
	 * unvmed_log_dump() merges them back into one time-ordered stream, so
	 * this still shows everything the way it always did.
	 */
	ret = unvmed_log_dump(stdout, UNVME_DAEMON_LOG, UNVME_DAEMON_TRACE,
			      arg_boolv(nvme) > 0, arg_boolv(follow) > 0);
	if (ret) {
		unvme_pr_err("failed to read unvme log files\n");
		ret = ENOENT;
	}

	unvme_free_args(argtable);
	return ret;
}
