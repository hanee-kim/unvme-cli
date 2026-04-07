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

#define UNVME_FIO_IOENGINE	"libunvmed-ioengine.so"
#define UNVME_FIO_CANCEL_TIMEOUT_SEC	5

/*
 * TLS pointer set to a jmp_buf just before calling fio main().  Our exit()
 * override below uses this to longjmp() back to the caller instead of
 * terminating the process.  It is NULL outside of an active fio run so that
 * normal (non-fio) exit() calls on any other thread are unaffected.
 */
static __thread jmp_buf *__fio_jump = NULL;

/*
 * Override exit() so that when fio (loaded as a shared library) calls exit()
 * from within a worker thread we can recover without killing the process.
 *
 * If __fio_jump is set we are inside unvmed_fio_run_thread(); longjmp() back
 * to the setjmp() checkpoint and let the thread clean up normally.
 *
 * Otherwise fall through to the real libc exit() so that nothing else in the
 * process is disrupted.
 */
void exit(int status)
{
	if (__fio_jump) {
		longjmp(*__fio_jump, EINTR);
		__builtin_unreachable();
	}

	/* Not inside a fio thread — call the real exit(). */
	void (*real_exit)(int) __attribute__((noreturn));
	*(void **)&real_exit = dlsym(RTLD_NEXT, "exit");
	if (real_exit)
		real_exit(status);

	/* Last-resort fallback if dlsym fails. */
	_exit(status);
}

struct unvmed_fio {
	pthread_t	thread;
	int		ret;
};

struct __fio_thread_args {
	const char	*libfio;
	int		 argc;
	char		**argv;
	int		 ret;
};

struct __fio_cleanup_ctx {
	void	**fio;
	void	**ioengine;
	char	***argv;
};

static void __fio_thread_cleanup(void *arg)
{
	struct __fio_cleanup_ctx *ctx = (struct __fio_cleanup_ctx *)arg;

	free(*ctx->argv);

	if (*ctx->fio)
		dlclose(*ctx->fio);
	if (*ctx->ioengine)
		dlclose(*ctx->ioengine);
}

static void __fio_reset(const char *libfio)
{
	void *handle;

	handle = dlopen(libfio, RTLD_NOLOAD);
	if (handle)
		dlclose(handle);

	handle = dlopen(UNVME_FIO_IOENGINE, RTLD_NOLOAD);
	if (handle)
		dlclose(handle);
}

static void *__fio_thread_run(void *opaque)
{
	struct __fio_thread_args *args = (struct __fio_thread_args *)opaque;
	int (*fio_main)(int, char *[], char *[]);
	const int nr_def_argc = 2;
	char **argv = NULL;
	void *fio = NULL;
	void *ioengine = NULL;
	jmp_buf jump;
	int ret = 0;

	extern char **environ;

	/*
	 * Register a cleanup handler so that dlopen'd handles are closed even
	 * when this thread is force-cancelled via pthread_cancel().  In all
	 * other exit paths we call pthread_cleanup_pop(0) and do the cleanup
	 * explicitly at the 'out' label below.
	 */
	struct __fio_cleanup_ctx cleanup_ctx = {
		.fio	 = &fio,
		.ioengine = &ioengine,
		.argv	 = &argv,
	};
	pthread_cleanup_push(__fio_thread_cleanup, &cleanup_ctx);

	/*
	 * Close any leftover handles from a previous run before re-loading.
	 * dlopen(RTLD_NOW | RTLD_GLOBAL) requires the same context that did
	 * the original open.
	 */
	__fio_reset(args->libfio);

	fio = dlopen(args->libfio, RTLD_NOW | RTLD_GLOBAL);
	if (!fio) {
		unvmed_log_err("failed to load fio shared object '%s': %s",
				args->libfio, dlerror());
		ret = -1;
		goto out;
	}

	ioengine = dlopen(UNVME_FIO_IOENGINE, RTLD_LAZY);
	if (!ioengine) {
		unvmed_log_err("failed to load ioengine '%s': %s",
				UNVME_FIO_IOENGINE, dlerror());
		ret = -1;
		goto out;
	}

	fio_main = dlsym(fio, "main");
	if (dlerror()) {
		unvmed_log_err("failed to resolve 'main' symbol in fio");
		ret = EINVAL;
		goto out;
	}

	/*
	 * Build argv: copy caller's args, then append mandatory defaults.
	 * --eta=always  : keep progress output flowing to stdio.
	 * --thread=1    : required by libunvmed-engine (uses pthreads, not fork).
	 * +1 for NULL terminator.
	 */
	argv = malloc(sizeof(char *) * (args->argc + nr_def_argc + 1));
	if (!argv) {
		ret = ENOMEM;
		goto out;
	}
	memcpy(argv, args->argv, sizeof(char *) * args->argc);
	argv[args->argc]     = "--eta=always";
	argv[args->argc + 1] = "--thread=1";
	argv[args->argc + 2] = NULL;

	/*
	 * If fio calls exit() internally (e.g., on SIGINT), our exit() override
	 * above will longjmp() here with EINTR so we can clean up properly
	 * instead of terminating the whole process.
	 */
	__fio_jump = &jump;
	ret = setjmp(*__fio_jump);
	if (ret == EINTR)
		goto out;

	ret = fio_main(args->argc + nr_def_argc, argv, environ);

out:
	__fio_jump = NULL;

	/* Normal exit: skip the cleanup handler and do it here. */
	pthread_cleanup_pop(0);

	free(argv);
	if (fio)
		dlclose(fio);
	if (ioengine)
		dlclose(ioengine);

	args->ret = ret;
	return NULL;
}

/**
 * unvmed_fio_run - Run fio as a background pthread
 * @libfio: path to the fio shared object (e.g., "/usr/lib/libfio.so")
 * @argc:   number of elements in @argv (not counting the NULL terminator)
 * @argv:   fio argument vector (argv[0] is the program name, as usual)
 *
 * Starts fio in a new thread.  The thread loads @libfio and the
 * libunvmed ioengine dynamically, then calls fio's main().
 *
 * Return: opaque &struct unvmed_fio handle on success, ``NULL`` on error
 * with ``errno`` set.  The caller must eventually pass the handle to either
 * unvmed_fio_wait() or unvmed_fio_cancel() to release resources.
 */
struct unvmed_fio *unvmed_fio_run(const char *libfio, int argc, char *argv[])
{
	struct unvmed_fio *fio;
	struct __fio_thread_args *args;

	fio = calloc(1, sizeof(*fio));
	if (!fio)
		return NULL;

	args = calloc(1, sizeof(*args));
	if (!args) {
		free(fio);
		return NULL;
	}

	args->libfio = libfio;
	args->argc   = argc;
	args->argv   = argv;

	if (pthread_create(&fio->thread, NULL, __fio_thread_run, args)) {
		free(args);
		free(fio);
		return NULL;
	}

	/* args is freed inside the thread after it finishes */
	fio->ret = 0;
	return fio;
}

/**
 * unvmed_fio_wait - Wait for a fio thread to finish naturally
 * @fio: handle returned by unvmed_fio_run()
 *
 * Blocks until the fio thread exits on its own.  Frees all resources
 * associated with @fio.
 *
 * Return: fio exit status (0 on success, non-zero otherwise).
 */
int unvmed_fio_wait(struct unvmed_fio *fio)
{
	struct __fio_thread_args *args = NULL;
	int ret;

	pthread_join(fio->thread, (void **)&args);
	ret = args ? args->ret : fio->ret;

	free(args);
	free(fio);
	return ret;
}

/**
 * unvmed_fio_cancel - Terminate a running fio thread
 * @fio: handle returned by unvmed_fio_run()
 *
 * Sends SIGINT to the fio thread so fio can shut down gracefully.  If the
 * thread does not exit within %UNVME_FIO_CANCEL_TIMEOUT_SEC seconds (e.g.,
 * it is stuck waiting for an NVMe completion), pthread_cancel() is called
 * as a last resort.  Cleanup handlers registered inside the thread ensure
 * that the dlopen'd libraries are closed even in the cancel path.
 *
 * Blocks until the thread has fully exited, then frees all resources.
 *
 * Return: fio exit status, or -ETIMEDOUT if the thread had to be
 * force-cancelled.
 */
int unvmed_fio_cancel(struct unvmed_fio *fio)
{
	struct __fio_thread_args *args = NULL;
	struct timespec ts;
	int ret;

	pthread_kill(fio->thread, SIGINT);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += UNVME_FIO_CANCEL_TIMEOUT_SEC;

	if (pthread_timedjoin_np(fio->thread, (void **)&args, &ts) == 0) {
		ret = args ? args->ret : fio->ret;
	} else {
		unvmed_log_err("fio thread did not exit within %d seconds, "
				"force-cancelling",
				UNVME_FIO_CANCEL_TIMEOUT_SEC);
		pthread_cancel(fio->thread);
		pthread_join(fio->thread, NULL);
		ret = -ETIMEDOUT;
	}

	free(args);
	free(fio);
	return ret;
}
