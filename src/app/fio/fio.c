// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <errno.h>
#include <setjmp.h>
#include <string.h>
#include <json-c/json.h>

#define UNVME_FIO_IOENGINE	"libunvmed-ioengine.so"
#define UNVME_FIO_LAST_REPORT	"/tmp/unvme-fio-last-report.json"

extern char **environ;
extern __thread jmp_buf *__jump;

extern char *unvme_get_filepath(char *pwd, const char *filename);

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

/*
 * Parse the fio JSON report at UNVME_FIO_LAST_REPORT and print a summary
 * table of per-job stats alongside any detected errors (non-zero job error
 * code, short_ios, or drop_ios).
 */
static void unvmed_fio_print_report(void)
{
	struct json_object *root, *jobs, *job, *val, *io_obj, *lat_ns;
	const char *io_types[] = {"read", "write", "trim"};
	bool has_error = false;
	int nr_jobs, i, t;

	root = json_object_from_file(UNVME_FIO_LAST_REPORT);
	if (!root) {
		fprintf(stderr, "[unvme] failed to parse fio report: %s\n",
			UNVME_FIO_LAST_REPORT);
		return;
	}

	printf("\n[unvme] fio report (saved: %s)\n", UNVME_FIO_LAST_REPORT);
	printf("%-24s  %-5s  %10s  %10s  %12s  %6s  %6s\n",
	       "job", "type", "IOPS", "BW(KiB/s)", "lat_avg(us)",
	       "short", "drop");
	printf("%-24s  %-5s  %10s  %10s  %12s  %6s  %6s\n",
	       "------------------------", "-----",
	       "----------", "----------", "------------",
	       "------", "------");

	if (!json_object_object_get_ex(root, "jobs", &jobs))
		goto out;

	nr_jobs = json_object_array_length(jobs);
	for (i = 0; i < nr_jobs; i++) {
		job = json_object_array_get_idx(jobs, i);

		const char *jobname = "unknown";
		if (json_object_object_get_ex(job, "jobname", &val))
			jobname = json_object_get_string(val);

		int job_error = 0;
		if (json_object_object_get_ex(job, "error", &val))
			job_error = json_object_get_int(val);

		for (t = 0; t < 3; t++) {
			int64_t total_ios = 0;
			double iops = 0.0, lat_mean_us = 0.0;
			int64_t bw = 0;
			int short_ios = 0, drop_ios = 0;

			if (!json_object_object_get_ex(job, io_types[t], &io_obj))
				continue;

			if (json_object_object_get_ex(io_obj, "total_ios", &val))
				total_ios = json_object_get_int64(val);
			if (total_ios == 0)
				continue;

			if (json_object_object_get_ex(io_obj, "iops", &val))
				iops = json_object_get_double(val);
			if (json_object_object_get_ex(io_obj, "bw", &val))
				bw = json_object_get_int64(val);
			if (json_object_object_get_ex(io_obj, "lat_ns", &lat_ns) &&
			    json_object_object_get_ex(lat_ns, "mean", &val))
				lat_mean_us = json_object_get_double(val) / 1000.0;
			if (json_object_object_get_ex(io_obj, "short_ios", &val))
				short_ios = json_object_get_int(val);
			if (json_object_object_get_ex(io_obj, "drop_ios", &val))
				drop_ios = json_object_get_int(val);

			bool row_error = job_error || short_ios || drop_ios;
			printf("%-24s  %-5s  %10.1f  %10" PRId64 "  %12.2f  %6d  %6d%s\n",
			       jobname, io_types[t], iops, bw, lat_mean_us,
			       short_ios, drop_ios,
			       row_error ? "  <-- ERROR" : "");

			if (row_error)
				has_error = true;
		}

		if (job_error)
			fprintf(stderr,
				"[unvme] job '%s' exited with error %d\n",
				jobname, job_error);
	}

	if (has_error)
		fprintf(stderr,
			"\n[unvme] Errors detected. Full report: %s\n",
			UNVME_FIO_LAST_REPORT);
	else
		printf("\n[unvme] No errors detected.\n");

out:
	json_object_put(root);
}

int unvmed_run_fio(int argc, char *argv[], const char *libfio, const char *pwd)
{
	int (*main)(int, char *[], char *[]);
	char **__argv;
	const int nr_def_argc = 2;
	int nr_extra_argc = 0;
	int ret = 0;
	bool has_output_format = false;
	bool has_output = false;

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
	 * Check whether the caller already specified --output-format or
	 * --output so we know which default args to inject below.
	 */
	for (int i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "--output-format", 15) ||
		    !strncmp(argv[i], "-output-format", 14))
			has_output_format = true;
		if (!strncmp(argv[i], "--output=", 9) ||
		    !strncmp(argv[i], "-output=", 8))
			has_output = true;
	}

	/*
	 * If the caller did not specify either flag we inject both so that the
	 * final report is always saved as JSON to UNVME_FIO_LAST_REPORT.
	 * Real-time progress is still visible via --eta=always (fio writes ETA
	 * lines directly to stderr / the controlling tty, independent of
	 * --output).
	 */
	if (!has_output_format)
		nr_extra_argc++;
	if (!has_output)
		nr_extra_argc++;

	/*
	 * Put a default argument '--eta=always' to print output in stdio
	 * successfully and '--thread=1' for those who forget to give this
	 * mandatory option for libunvmed ioengine.
	 * The last +1 is for NULL.
	 */
	__argv = malloc(sizeof(char *) * (argc + nr_def_argc + nr_extra_argc + 1));
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

	int idx = argc;
	__argv[idx++] = "--eta=always";
	__argv[idx++] = "--thread=1";
	if (!has_output_format)
		__argv[idx++] = "--output-format=json";
	if (!has_output)
		__argv[idx++] = "--output=" UNVME_FIO_LAST_REPORT;
	__argv[idx] = NULL;

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

	ret = main(argc + nr_def_argc + nr_extra_argc, __argv, environ);

out:
	free(__argv);

	if (!has_output_format && !has_output)
		unvmed_fio_print_report();

	dlclose(fio);
	dlclose(ioengine);

	return ret;
}
