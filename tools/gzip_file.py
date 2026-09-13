#!/usr/bin/env python3
"""Compress one file, deterministically, for embedding in the firmware.

Used at build time on the console page. Python rather than the gzip binary so
the build does not depend on a shell tool being present, and mtime is pinned to
0 so two builds of the same source produce byte-identical output -- a build
that differs run to run makes "did this change?" unanswerable.
"""

import gzip
import shutil
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gzip_file.py <in> <out>")
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as fin, open(dst, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw,
                           compresslevel=9, mtime=0) as fout:
            shutil.copyfileobj(fin, fout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
