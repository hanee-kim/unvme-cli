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
#define UNVME_FIO_LAST_REPORT	"/tmp/unvme-fio-last-report"

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
 * Print the normal-text portion of the combined json+normal report to stdout,
 * then parse the JSON block that follows and report any errors.
 *
 * fio with --output-format=json+normal writes the human-readable summary first
 * and then a pretty-printed JSON object (starting with "{\n") at the end of
 * the file.  We split on that boundary so the user sees the familiar fio
 * output while we still get a machine-readable blob for error extraction.
 */
static void unvmed_fio_print_report(void)
{
	struct json_object *root, *jobs, *job, *val, *io_obj;
	const char *io_types[] = {"read", "write", "trim"};
	char *line = NULL;
	char *json_buf = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	long json_start = -1;
	long filesize, jsonsize;
	size_t nread;
	bool has_error = false;
	int nr_jobs, i, t;
	FILE *fp;

	fp = fopen(UNVME_FIO_LAST_REPORT, "r");
	if (!fp) {
		fprintf(stderr, "[unvme] failed to open fio report: %s\n",
			UNVME_FIO_LAST_REPORT);
		return;
	}

	/*
	 * Stream the normal-text section to stdout line by line.  Stop when we
	 * hit the opening "{" of the JSON block (always on its own line in
	 * fio's pretty-printed output) and remember the file offset.
	 */
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		if (line[0] == '{' && (line[1] == '\n' || line[1] == '\0')) {
			json_start = ftell(fp) - linelen;
			break;
		}
		fwrite(line, 1, linelen, stdout);
	}
	fflush(stdout);
	free(line);

	if (json_start < 0) {
		fclose(fp);
		return;
	}

	/* Read the JSON portion into a buffer for parsing. */
	fseek(fp, 0, SEEK_END);
	filesize = ftell(fp);
	jsonsize = filesize - json_start;

	json_buf = malloc(jsonsize + 1);
	if (!json_buf) {
		fclose(fp);
		return;
	}

	fseek(fp, json_start, SEEK_SET);
	nread = fread(json_buf, 1, jsonsize, fp);
	json_buf[nread] = '\0';
	fclose(fp);

	root = json_tokener_parse(json_buf);
	free(json_buf);
	if (!root) {
		fprintf(stderr, "[unvme] failed to parse fio JSON report\n");
		return;
	}

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
			int short_ios = 0, drop_ios = 0;

			if (!json_object_object_get_ex(job, io_types[t], &io_obj))
				continue;
			if (json_object_object_get_ex(io_obj, "total_ios", &val))
				total_ios = json_object_get_int64(val);
			if (total_ios == 0)
				continue;
			if (json_object_object_get_ex(io_obj, "short_ios", &val))
				short_ios = json_object_get_int(val);
			if (json_object_object_get_ex(io_obj, "drop_ios", &val))
				drop_ios = json_object_get_int(val);

			if (job_error || short_ios || drop_ios) {
				if (!has_error) {
					fprintf(stderr,
						"\n[unvme] Errors detected:\n");
					has_error = true;
				}
				fprintf(stderr,
					"  job=%-20s  %-5s  error=%d"
					"  short_ios=%d  drop_ios=%d\n",
					jobname, io_types[t], job_error,
					short_ios, drop_ios);
			}
		}
	}

	if (has_error)
		fprintf(stderr, "[unvme] Full report: %s\n",
			UNVME_FIO_LAST_REPORT);
	else
		printf("[unvme] No errors detected. Report: %s\n",
		       UNVME_FIO_LAST_REPORT);

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
	 * If the caller did not specify either flag, inject --output-format=
	 * json+normal and --output=UNVME_FIO_LAST_REPORT.  fio writes the
	 * human-readable summary followed by the JSON block into that file.
	 * Real-time ETA progress is unaffected because fio emits those lines
	 * directly to the controlling tty regardless of --output.
	 * unvmed_fio_print_report() re-emits the normal portion to stdout and
	 * parses the JSON block for errors after fio exits.
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
		__argv[idx++] = "--output-format=json+normal";
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
