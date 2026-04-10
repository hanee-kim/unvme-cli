# SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
# cython: language_level=3
"""
Cython wrapper for the libunvmed fio thread-runner API.

Typical usage::

    import unvmed_fio

    unvmed_fio.set_libpath("/usr/lib/libfio.so")
    unvmed_fio.run("test.fio",
                   opts="--eta=always, --eta-interval=1, --output-format=normal")

    while True:
        is_done, ret = unvmed_fio.done()
        if is_done:
            break
        time.sleep(0.1)

Or to abort early::

    ret = unvmed_fio.cancel()
"""

from libc.stdlib cimport malloc, free

cdef extern from "libunvmed.h":
    void unvmed_fio_set_libpath(const char *libfio)
    int  unvmed_fio_run(char *jobfile, int nr_extra, char **extra)
    bint unvmed_fio_done(int *ret)
    int  unvmed_fio_cancel()

# ---------------------------------------------------------------------------
# Module-level state pinned for the duration of each fio run.
#
# unvmed_fio_run() is non-blocking: it spawns a pthread and returns
# immediately.  The char* strings in the argv it builds must remain valid
# until the thread exits (i.e. until done() or cancel() returns).
#
# Strategy:
#   - _g_jobfile_b and _g_extra_list hold Python bytes objects.
#     As long as these globals reference them the GC cannot collect them,
#     so the underlying char* buffers stay valid.
#   - _g_extra_c is the malloc'd char** array that points into those buffers.
#     It is freed by _release() once the thread has exited.
# ---------------------------------------------------------------------------
cdef char **_g_extra_c = NULL   # malloc'd array of char*
_g_jobfile_b  = None            # bytes for the jobfile path
_g_extra_list = []              # list[bytes] for each extra option string


cdef inline void _release() noexcept:
    """Free C resources; called after the fio thread has finished."""
    global _g_extra_c
    if _g_extra_c != NULL:
        free(_g_extra_c)
        _g_extra_c = NULL


def _release_py():
    """Drop Python-side pins; called together with _release()."""
    global _g_extra_list, _g_jobfile_b
    _g_extra_list = []
    _g_jobfile_b  = None


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

__all__ = ["set_libpath", "run", "done", "cancel"]


def set_libpath(str path):
    """
    Set the path to fio built as a shared library.

    Must be called once before :func:`run`.

    :param path: Filesystem path to ``libfio.so`` (or equivalent).
    """
    unvmed_fio_set_libpath(path.encode())


def run(str jobfile, str opts=None):
    """
    Start fio with *jobfile* in a background thread.

    :param jobfile:
        Path to the fio job file.
    :param opts:
        Comma-separated fio CLI option string, e.g.::

            "--eta=always, --eta-interval=1, --output-format=normal"

        Each token is stripped of surrounding whitespace before being
        passed to fio.  Pass ``None`` (default) to use the libunvmed
        default (``--eta=always``).

    ``--thread=1`` is always appended automatically (required by the
    libunvmed ioengine).

    :returns: ``0`` on success.
    :raises OSError: if fio fails to start.
    :raises MemoryError: if memory allocation fails.
    """
    global _g_extra_c, _g_extra_list, _g_jobfile_b

    cdef int   ret      = 0
    cdef int   nr_extra = 0
    cdef int   i
    cdef bytes bitem

    # Drop any leftover state from a previous run.
    _release()
    _release_py()

    # Parse the comma-separated opts string into individual tokens.
    extra_strs = []
    if opts:
        extra_strs = [o.strip() for o in opts.split(',') if o.strip()]

    # Pin bytes objects so the GC cannot collect them while the thread runs.
    _g_jobfile_b  = jobfile.encode()
    _g_extra_list = [o.encode() for o in extra_strs]
    nr_extra      = len(_g_extra_list)

    # Build char** array pointing into the pinned bytes buffers.
    if nr_extra > 0:
        _g_extra_c = <char **>malloc(nr_extra * sizeof(char *))
        if _g_extra_c == NULL:
            _g_extra_list = []
            _g_jobfile_b  = None
            raise MemoryError()
        for i in range(nr_extra):
            bitem         = _g_extra_list[i]
            _g_extra_c[i] = bitem

    cdef bytes jfb = _g_jobfile_b
    ret = unvmed_fio_run(jfb, nr_extra, _g_extra_c)
    if ret < 0:
        _release()
        _release_py()
        raise OSError(-ret, f'unvmed_fio_run() failed (errno {-ret})')

    return ret


def done():
    """
    Non-blocking poll: check whether fio has finished.

    :returns: ``(is_done: bool, exit_code: int)``.  When *is_done* is
              ``True`` the exit code carries fio's return value and all
              internal resources are released automatically.
    """
    cdef int  c_ret    = 0
    cdef bint finished = unvmed_fio_done(&c_ret)
    if finished:
        _release()
        _release_py()
    return bool(finished), c_ret


def cancel():
    """
    Terminate the running fio thread.

    Sends ``SIGINT`` to fio and waits for it to exit cleanly.  Falls back
    to ``pthread_cancel()`` if fio does not exit within the cancellation
    timeout.  Always releases internal resources before returning.

    :returns: fio's exit status.
    """
    cdef int ret = unvmed_fio_cancel()
    _release()
    _release_py()
    return ret

