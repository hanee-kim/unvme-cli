// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <setjmp.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#define UNVME_FIO_IOENGINE	"libunvmed-ioengine.so"

extern char **environ;
extern __thread jmp_buf *__jump;

extern const char *unvmed_get_libfio(void);
extern char *unvme_get_filepath(char *pwd, const char *filename);

/*
 * Parse a whitespace-separated options string into an argv array.
 * Caller must free each element and the array itself.
 * Returns the number of tokens parsed.
 */
static int parse_opts_to_argv(const char *opts, char ***out_argv)
{
	char *opts_copy;
	char *token;
	char **argv = NULL;
	int argc = 0;
	int capacity = 0;

	if (!opts || opts[0] == '\0') {
		*out_argv = NULL;
		return 0;
	}

	opts_copy = strdup(opts);
	if (!opts_copy)
		return -ENOMEM;

	token = strtok(opts_copy, " \t\n");
	while (token) {
		if (argc >= capacity) {
			capacity = capacity ? capacity * 2 : 8;
			argv = realloc(argv, sizeof(char *) * capacity);
			if (!argv) {
				free(opts_copy);
				return -ENOMEM;
			}
		}
		argv[argc++] = strdup(token);
		token = strtok(NULL, " \t\n");
	}

	free(opts_copy);
	*out_argv = argv;
	return argc;
}

static void unvmed_fio_reset(const char *libfio)
{
	void *handle;

	handle = dlopen(libfio, RTLD_NOLOAD);
	if (handle)
		dlclose(handle);

	handle = dlopen(UNVME_FIO_IOENGINE, RTLD_NOLOAD);
	if (handle)
		dlclose(handle);
}

int unvmed_run_fio(int argc, char *argv[], const char *libfio, const char *pwd)
{
	int (*main)(int, char *[], char *[]);
	char **__argv;
	const int nr_def_argc = 2;
	int ret = 0;

	void *fio;
	void *ioengine;

	jmp_buf jump;

	/*
	 * If the previous app handle has not been closed yet, close here
	 * rather than closing it from the pthread context.  It should be the
	 * same context where dlopen() actually happened.
	 */
	unvmed_fio_reset(libfio);

	/*
	* Load fio binary built as a shared obejct every time `unvme fio`
	* command is invoked.  By freshly reloading the fio code and data to
	* the memory makes fio run as a standalone application.
	*
	* Open fio shared object with RLTD_NOW | RTLD_GLOBAL to guarantee that
	* the following ioengine shared object can refer the fio symbols
	* properly.
	*/
	fio = dlopen(libfio, RTLD_NOW | RTLD_GLOBAL);
	if (!fio) {
		fprintf(stderr, "failed to load shared object '%s'.  "
				"Give proper path to 'unvme start --with-fio=<path/to/fio/so>'\n", libfio);
		return -1;
	}

	/*
	 * Once ioengine shared object is loaded into the current process
	 * context, constructor will be called registering the ioengine to the
	 * fio context.
	 */
	ioengine = dlopen(UNVME_FIO_IOENGINE, RTLD_LAZY);
	if (!ioengine) {
		fprintf(stderr, "failed to load ioengine %s\n", UNVME_FIO_IOENGINE);
		return -1;
	}

	main = dlsym(fio, "main");
	if (dlerror()) {
		fprintf(stderr, "failed to load 'main' symbol in fio. "
				"Maybe forgot to give 'unvme start --with-fio=<path/to/fio/so>'\n");
		return errno;
	}

	/*
	 * Put a default argument '--eta=always' to print output in stdio
	 * successfully and '--thread=1' for those who forget to give this
	 * mandatory option for libunvmed ioengine.
	 * The last +1 is for NULL.
	 */
	__argv = malloc(sizeof(char *) * (argc + nr_def_argc + 1));
	for (int i = 0; i < argc; i++) {
		/* job file path */
		if (argv[i][0] != '-')
			__argv[i] = unvme_get_filepath((char *)pwd, argv[i]);
		else if (!strncmp(argv[i], "-output=", 8) ||
				!strncmp(argv[i], "--output=", 9)) {
			char *val = strstr(argv[i], "=");
			char *output = unvme_get_filepath((char *)pwd, ++val);

			if (asprintf(&__argv[i], "--output=%s", output) < 0) {
				fprintf(stderr, "failed to form --output=\n");
				return errno;
			}
		} else
			__argv[i] = argv[i];
	}
	__argv[argc] = "--eta=always";
	__argv[argc + 1] = "--thread=1";
	__argv[argc + 2] = NULL;

	/*
	 * If fio is terminated by exit() call inside of the shared object,
	 * dynamically loaded code and data won't be cleaned up properly.  It
	 * causes the next load not to initialize the global variables properly
	 * causing an initialization failure from the second run.  To fix this,
	 * if fio calls exit(), we catch the exit() call in our daemon exit()
	 * symbol and our exit() will longjmp() to here with EINTR value to
	 * indicate that this has been terminated.  If so, we can move the
	 * instruction to the 'out' label and clean up all the mess.
	 */
	__jump = &jump;
	ret = setjmp(*__jump);
	if (ret == EINTR)
		goto out;

	ret = main(argc + nr_def_argc, __argv, environ);

out:
	free(__argv);

	dlclose(fio);
	dlclose(ioengine);

	return ret;
}

/*
 * unvmed_fio_run - run fio with a job file and a CLI options string.
 *
 * @jobfile: path to the fio job file
 * @opts:    space-separated fio CLI options
 *           e.g. "--eta=always --eta-interval=1 --output-format=json"
 *
 * libfio is resolved via unvmed_get_libfio(); falls back to "fio.so".
 * pwd is set to the current working directory.
 * "--thread=1" is always appended to satisfy the libunvmed ioengine
 * requirement unless the caller already includes it in @opts.
 */
int unvmed_fio_run(const char *jobfile, const char *opts)
{
	const char *libfio;
	char pwd[PATH_MAX];
	char **opts_argv = NULL;
	char **argv = NULL;
	int opts_argc;
	int total_argc;
	int ret;

	libfio = unvmed_get_libfio();
	if (!libfio)
		libfio = "fio.so";

	if (!getcwd(pwd, sizeof(pwd))) {
		perror("getcwd");
		return -errno;
	}

	opts_argc = parse_opts_to_argv(opts, &opts_argv);
	if (opts_argc < 0)
		return opts_argc;

	/*
	 * Build argv as: [jobfile, <opts tokens...>, "--thread=1"]
	 * --thread=1 is mandatory for the libunvmed ioengine.
	 */
	total_argc = 1 + opts_argc + 1;
	argv = malloc(sizeof(char *) * total_argc);
	if (!argv) {
		ret = -ENOMEM;
		goto free_opts;
	}

	argv[0] = unvme_get_filepath(pwd, jobfile);
	for (int i = 0; i < opts_argc; i++)
		argv[1 + i] = opts_argv[i];
	argv[1 + opts_argc] = "--thread=1";

	ret = unvmed_run_fio(total_argc, argv, libfio, pwd);

	free(argv[0]);
	free(argv);
free_opts:
	for (int i = 0; i < opts_argc; i++)
		free(opts_argv[i]);
	free(opts_argv);

	return ret;
}
