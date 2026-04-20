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

static pthread_t	__thread;
static bool		__running = false;
static int		__retval  = 0;

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
 * If __fio_jump is set we are inside __fio_thread_run(); longjmp() back to
 * the setjmp() checkpoint and let the thread clean up normally.
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

	void (*real_exit)(int) __attribute__((noreturn));
	*(void **)&real_exit = dlsym(RTLD_NEXT, "exit");
	if (real_exit)
		real_exit(status);

	_exit(status);
}

struct __fio_thread_args {
	const char	*libfio;
	int		 argc;
	char		**argv;
};

struct __fio_cleanup_ctx {
	void		**fio;
	void		**ioengine;
	char		***argv;
	struct __fio_thread_args **args;
};

static void __fio_thread_cleanup(void *arg)
{
	struct __fio_cleanup_ctx *ctx = (struct __fio_cleanup_ctx *)arg;

	free(*ctx->argv);
	free(*ctx->args);

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

	struct __fio_cleanup_ctx cleanup_ctx = {
		.fio      = &fio,
		.ioengine = &ioengine,
		.argv     = &argv,
		.args     = &args,
	};
	pthread_cleanup_push(__fio_thread_cleanup, &cleanup_ctx);

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

	argv = malloc(sizeof(char *) * (args->argc + nr_def_argc + 1));
	if (!argv) {
		ret = ENOMEM;
		goto out;
	}
	memcpy(argv, args->argv, sizeof(char *) * args->argc);
	argv[args->argc]     = "--eta=always";
	argv[args->argc + 1] = "--thread=1";
	argv[args->argc + 2] = NULL;

	__fio_jump = &jump;
	ret = setjmp(*__fio_jump);
	if (ret == EINTR)
		goto out;

	ret = fio_main(args->argc + nr_def_argc, argv, environ);

out:
	__fio_jump = NULL;

	pthread_cleanup_pop(0);

	free(argv);
	free(args);
	if (fio)
		dlclose(fio);
	if (ioengine)
		dlclose(ioengine);

	return (void *)(intptr_t)ret;
}

/**
 * unvmed_fio_run - Run fio as a background pthread
 * @libfio: path to the fio shared object (e.g., "/usr/lib/libfio.so")
 * @argc:   number of elements in @argv (not counting the NULL terminator)
 * @argv:   fio argument vector (argv[0] is the program name, as usual)
 *
 * Starts fio in a new thread.  Only one fio instance may run at a time.
 *
 * Return: ``0`` on success, ``-EBUSY`` if already running, ``-1`` on error.
 */
int unvmed_fio_run(const char *libfio, int argc, char *argv[])
{
	struct __fio_thread_args *args;

	if (__running) {
		unvmed_log_err("fio is already running");
		return -EBUSY;
	}

	args = calloc(1, sizeof(*args));
	if (!args)
		return -1;

	args->libfio = libfio;
	args->argc   = argc;
	args->argv   = argv;

	__retval  = 0;
	__running = true;

	if (pthread_create(&__thread, NULL, __fio_thread_run, args)) {
		__running = false;
		free(args);
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
	void *retval;
	int __ret;

	if (!__running) {
		if (ret)
			*ret = __retval;
		return true;
	}

	__ret = pthread_tryjoin_np(__thread, &retval);
	if (__ret)
		return false;

	__running = false;
	__retval  = (int)(intptr_t)retval;
	if (ret)
		*ret = __retval;
	return true;
}

/**
 * unvmed_fio_cancel - Terminate the running fio thread
 *
 * Sends SIGINT to the fio thread so fio can shut down gracefully.  If the
 * thread does not exit within %UNVME_FIO_CANCEL_TIMEOUT_SEC seconds,
 * pthread_cancel() is called as a last resort.
 *
 * Blocks until the thread has fully exited.
 *
 * Return: fio exit status, or ``-ETIMEDOUT`` if force-cancelled.
 */
int unvmed_fio_cancel(void)
{
	struct timespec ts;
	void *retval;

	if (!__running)
		return __retval;

	pthread_kill(__thread, SIGINT);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += UNVME_FIO_CANCEL_TIMEOUT_SEC;

	if (pthread_timedjoin_np(__thread, &retval, &ts) == 0) {
		__running = false;
		__retval  = (int)(intptr_t)retval;
		return __retval;
	}

	unvmed_log_err("fio thread did not exit within %d seconds, "
			"force-cancelling", UNVME_FIO_CANCEL_TIMEOUT_SEC);

	pthread_cancel(__thread);
	pthread_join(__thread, NULL);
	__running = false;
	return -ETIMEDOUT;
}
