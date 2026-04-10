# SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
"""
Build the unvmed_fio Cython extension.

Typical workflow (from this directory):

    # Build libunvmed first (adjust path as needed):
    #   cd .. && meson setup build && ninja -C build

    pip install cython
    python setup.py build_ext --inplace \
        --include-dirs=../lib \
        --library-dirs=../build/lib \
        --rpath=../build/lib

Or pass the paths via environment variables / pip editable install.
"""

import os
from setuptools import setup, Extension
from Cython.Build import cythonize

# Allow overriding paths via environment variables so this script works
# without modification regardless of where libunvmed is installed.
_root   = os.path.dirname(os.path.abspath(__file__))
_libdir = os.environ.get("LIBUNVMED_LIBDIR",  os.path.join(_root, "..", "build", "lib"))
_incdir = os.environ.get("LIBUNVMED_INCDIR",  os.path.join(_root, "..", "lib"))

ext = Extension(
    name="unvmed_fio",
    sources=["unvmed_fio.pyx"],
    include_dirs=[_incdir],
    library_dirs=[_libdir],
    libraries=["unvmed"],
    runtime_library_dirs=[os.path.abspath(_libdir)],
)

setup(
    name="unvmed-fio",
    version="1.0.0",
    description="Cython bindings for the libunvmed fio thread-runner API",
    ext_modules=cythonize(
        [ext],
        compiler_directives={"language_level": "3"},
    ),
)
