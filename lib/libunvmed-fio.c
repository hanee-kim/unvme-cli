// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <time.h>
#include <pthread.h>

#include "libunvmed.h"
#include "libunvmed-logs.h"

#define UNVME_FIO_IOENGINE		"libunvmed-ioengine.so"
#define UNVME_FIO_CANCEL_TIMEOUT_SEC	5

/*
 * Global fio state.  Only one fio instance runs at a time.
 *
 * g_fio / g_ioengine are kept open across runs (re-opened on each
 * unvmed_fio_run() call via RTLD_NOLOAD + dlclose before re-loading) so that
 * the same context that did dlopen() always does dlclose().
 */
static const char	*g_libfio_path;	/* set by unvmed_fio_set_libpath() */
static void		*g_fio;		/* dlopen handle for fio shared object */
static void		*g_ioengine;	/* dlopen handle for ioengine */
static pthread_t	 g_thread;
static bool		 g_running;
static int		 g_ret;

/*
 * TLS pointer set just before calling fio main().  Our exit() override uses
 * it to longjmp() back to the setjmp() checkpoint in the fio thread instead
 * of terminating the process.  NULL outside of an active fio run, so all
 * other threads (e.g., Python's main thread) see the real libc exit().
 */
static __thread jmp_buf *__fio_jump;

/*
 * Override exit() to prevent fio from killing the whole process when it
 * handles SIGINT.  Only active in the fio worker thread (__fio_jump != NULL).
 * All other callers are forwarded to the real libc exit() via dlsym(RTLD_NEXT).
 */
void exit(int status)
{
	if (__fio_jump) {
		longjmp(*__fio_jump, EINTR);
		__builtin_unreachable();
	}

	void (*real_exit)(int) __attribute__((noreturn));
	*(void **)&real_exit = dlsym(RTLD_NEXT, "exit");
	if (real_exit)
		real_exit(status);

	_exit(status);
}

/* pthread_cleanup handler: close dlopen'd handles on pthread_cancel(). */
struct __fio_cleanup_ctx {
	char **argv;
};

static void __fio_cleanup(void *arg)
{
	struct __fio_cleanup_ctx *ctx = arg;

	free(ctx->argv);

	if (g_ioengine) {
		dlclose(g_ioengine);
		g_ioengine = NULL;
	}
	if (g_fio) {
		dlclose(g_fio);
		g_fio = NULL;
	}
}

static void __fio_reset(void)
{
	void *h;

	h = dlopen(g_libfio_path, RTLD_NOLOAD);
	if (h)
		dlclose(h);

	h = dlopen(UNVME_FIO_IOENGINE, RTLD_NOLOAD);
	if (h)
		dlclose(h);
}

static void *__fio_thread_run(void *opaque)
{
	char *jobfile = (char *)opaque;
	int (*fio_main)(int, char *[], char *[]);
	char **argv = NULL;
	jmp_buf jump;
	int ret = 0;

	extern char **environ;

	struct __fio_cleanup_ctx ctx = { .argv = NULL };
	pthread_cleanup_push(__fio_cleanup, &ctx);

	__fio_reset();

	g_fio = dlopen(g_libfio_path, RTLD_NOW | RTLD_GLOBAL);
	if (!g_fio) {
		unvmed_log_err("failed to load fio '%s': %s",
				g_libfio_path, dlerror());
		ret = -1;
		goto out;
	}

	g_ioengine = dlopen(UNVME_FIO_IOENGINE, RTLD_LAZY);
	if (!g_ioengine) {
		unvmed_log_err("failed to load ioengine '%s': %s",
				UNVME_FIO_IOENGINE, dlerror());
		ret = -1;
		goto out;
	}

	fio_main = dlsym(g_fio, "main");
	if (dlerror()) {
		unvmed_log_err("failed to resolve 'main' in fio");
		ret = EINVAL;
		goto out;
	}

	/*
	 * argv[0]  : program name ("fio")
	 * argv[1]  : jobfile path
	 * argv[2]  : --eta=always  (keep progress output flowing)
	 * argv[3]  : --thread=1    (required by libunvmed-engine)
	 * argv[4]  : NULL
	 */
	argv = malloc(sizeof(char *) * 5);
	if (!argv) {
		ret = ENOMEM;
		goto out;
	}
	ctx.argv   = argv;
	argv[0]    = "fio";
	argv[1]    = jobfile;
	argv[2]    = "--eta=always";
	argv[3]    = "--thread=1";
	argv[4]    = NULL;

	/*
	 * If fio calls exit() (e.g., on SIGINT), our exit() override above
	 * longjmp()s here with EINTR so we can clean up without killing the
	 * process.
	 */
	__fio_jump = &jump;
	ret = setjmp(*__fio_jump);
	if (ret == EINTR)
		goto out;

	ret = fio_main(4, argv, environ);

out:
	__fio_jump = NULL;

	/* Normal / longjmp exit: skip cleanup handler, do it ourselves. */
	pthread_cleanup_pop(0);

	free(argv);

	if (g_ioengine) {
		dlclose(g_ioengine);
		g_ioengine = NULL;
	}
	if (g_fio) {
		dlclose(g_fio);
		g_fio = NULL;
	}

	g_ret = ret;
	return (void *)(intptr_t)ret;
}

/**
 * unvmed_fio_set_libpath - Set the path to the fio shared library
 * @libfio: path to fio built as a shared object (e.g. "/usr/lib/libfio.so")
 *
 * Must be called once before the first unvmed_fio_run() call.
 */
void unvmed_fio_set_libpath(const char *libfio)
{
	g_libfio_path = libfio;
}

/**
 * unvmed_fio_run - Run fio with the given job file in a background thread
 * @jobfile: path to the fio job file
 *
 * Starts fio in a new pthread.  Only one fio instance may run at a time;
 * returns -EBUSY if a run is already in progress.
 *
 * Return: ``0`` on success, ``-EBUSY`` if already running, ``-1`` on error.
 */
int unvmed_fio_run(char *jobfile)
{
	if (g_running) {
		unvmed_log_err("fio is already running");
		return -EBUSY;
	}

	if (!g_libfio_path) {
		unvmed_log_err("libfio path not set; call unvmed_fio_set_libpath() first");
		return -EINVAL;
	}

	g_ret     = 0;
	g_running = true;

	if (pthread_create(&g_thread, NULL, __fio_thread_run, jobfile)) {
		g_running = false;
		return -1;
	}

	return 0;
}

/**
 * unvmed_fio_done - Non-blocking check whether fio has finished
 * @ret: if non-NULL and fio is done, receives the fio exit status
 *
 * Returns ``true`` and sets @ret when fio has completed (or was never
 * started).  Returns ``false`` while fio is still running.
 */
bool unvmed_fio_done(int *ret)
{
	if (!g_running) {
		if (ret)
			*ret = g_ret;
		return true;
	}

	void *retval;

	if (pthread_tryjoin_np(g_thread, &retval) != 0)
		return false;

	g_running = false;
	if (ret)
		*ret = (int)(intptr_t)retval;
	return true;
}

/**
 * unvmed_fio_cancel - Terminate the running fio thread
 *
 * Sends SIGINT to the fio thread so fio can shut down gracefully via its
 * own signal handler.  If the thread does not exit within
 * %UNVME_FIO_CANCEL_TIMEOUT_SEC seconds (e.g., stuck waiting for an NVMe
 * completion), pthread_cancel() is called as a last resort; the cleanup
 * handler ensures dlopen'd libraries are closed even in that path.
 *
 * Blocks until the thread has fully exited.
 *
 * Return: fio exit status, or ``-ETIMEDOUT`` if force-cancelled.
 */
int unvmed_fio_cancel(void)
{
	void (*engine_cancel)(void);
	struct timespec ts;
	void *retval;

	if (!g_running)
		return g_ret;

	pthread_kill(g_thread, SIGINT);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += UNVME_FIO_CANCEL_TIMEOUT_SEC;

	if (pthread_timedjoin_np(g_thread, &retval, &ts) == 0) {
		g_running = false;
		return (int)(intptr_t)retval;
	}

	/*
	 * Timed out: job threads are stuck in the CQ spin loop waiting for
	 * an NVMe completion that never arrives.  Cancel them one by one via
	 * the engine; pthread_testcancel() inside the spin loop makes deferred
	 * cancellation take effect there.  Once all job threads are gone the
	 * main fio thread can exit and pthread_join returns.
	 */
	unvmed_log_err("fio thread did not exit within %d seconds, "
			"cancelling job threads", UNVME_FIO_CANCEL_TIMEOUT_SEC);

	*(void **)&engine_cancel = dlsym(g_ioengine, "unvmed_fio_cancel_threads");
	if (engine_cancel)
		engine_cancel();

	/*
	 * After job threads exit, fio's main thread proceeds to cleanup and
	 * calls wait_for_helper_thread(), which signals hd->done and joins
	 * the helper thread.  If the helper thread is stuck (e.g., performing
	 * disk-utilization I/O), fio_terminate_helper_thread() kicks it out
	 * of its pthread_cond_timedwait() loop directly so that the join
	 * inside wait_for_helper_thread() can return and fio_main() can exit.
	 */
	void (*term_helper)(void);
	*(void **)&term_helper = dlsym(g_fio, "fio_terminate_helper_thread");
	if (term_helper)
		term_helper();

	/* Give fio time to join the helper thread and exit normally. */
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 2;

	if (pthread_timedjoin_np(g_thread, &retval, &ts) == 0) {
		g_running = false;
		return (int)(intptr_t)retval;
	}

	unvmed_log_err("fio and its internal threads (job threads, helper thread) "
			"did not terminate within the grace period after SIGINT. "
			"This typically means NVMe completions never arrived (device "
			"unresponsive or controller reset) so job threads remained stuck "
			"in the CQ spin loop even after pthread_cancel(), and fio's main "
			"thread could not finish cleanup.  Killing the process.");
	pthread_cancel(g_thread);
	pthread_join(g_thread, &retval);

	g_running = false;
	return (int)(intptr_t)retval;
}
