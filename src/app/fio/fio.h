/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNVME_FIO_H
#define UNVME_FIO_H

/*
 * unvmed_run_fio - low-level fio runner; takes pre-split argc/argv.
 */
int unvmed_run_fio(int argc, char *argv[], const char *libfio, const char *pwd);

/*
 * unvmed_fio_run - high-level fio runner that accepts a job file path and
 *                  a space-separated CLI options string.
 *
 * @jobfile: path to the fio job file
 * @opts:    fio CLI options string
 *           e.g. "--eta=always --eta-interval=1 --output-format=json"
 */
int unvmed_fio_run(const char *jobfile, const char *opts);

#endif /* UNVME_FIO_H */
