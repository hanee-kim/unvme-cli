# SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
#
# Cython C-level declarations for the libunvmed fio thread-runner API.
# Import this .pxd in other .pyx files to reuse the C declarations without
# re-including the header.

cdef extern from "libunvmed.h":
    void unvmed_fio_set_libpath(const char *libfio)
    int  unvmed_fio_run(char *jobfile, int nr_extra, char **extra)
    bint unvmed_fio_done(int *ret)   # bool → bint
    int  unvmed_fio_cancel()
