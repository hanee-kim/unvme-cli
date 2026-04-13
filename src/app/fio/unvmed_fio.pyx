# SPDX-License-Identifier: GPL-2.0-only
# cython: language_level=3

"""
Cython wrapper for running fio via the unvmed ioengine.

Usage::

    from unvmed_fio import unvmed_fio_run

    ret = unvmed_fio_run(
        jobfile="my_test.fio",
        opts="--eta=always --eta-interval=1 --output-format=json",
    )
"""

cdef extern from "fio.h" nogil:
    int _unvmed_fio_run "unvmed_fio_run"(const char *jobfile, const char *opts)


def unvmed_fio_run(str jobfile, str opts):
    """Run fio with the given job file and CLI options string.

    Parameters
    ----------
    jobfile:
        Path to the fio job file.
    opts:
        Space-separated fio CLI options, e.g.
        ``"--eta=always --eta-interval=1 --output-format=json"``.
        ``--thread=1`` is always appended automatically to satisfy the
        libunvmed ioengine requirement.

    Returns
    -------
    int
        fio exit code (0 on success).
    """
    cdef bytes jobfile_b = jobfile.encode("utf-8")
    cdef bytes opts_b = opts.encode("utf-8") if opts else b""

    return _unvmed_fio_run(jobfile_b, opts_b)
