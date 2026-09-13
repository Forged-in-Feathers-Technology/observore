#!/usr/bin/env python3
"""Collect one board's flashable parts, with the offsets the build chose.

The offsets are read from build/flash_args rather than written down here, and
that is the whole point of this script.  They are not the same on every chip:
the S3 puts its bootloader at 0x0 and the C5 puts it at 0x2000.  A manifest
that hardcodes one chip's layout produces a flash that does not boot, with no
error at flashing time to say why -- so the numbers come from the build that
produced the binaries, or they do not come at all.
"""

import argparse
import json
import os
import shutil
import sys

CHIP_FAMILY = {
    "esp32s3": "ESP32-S3",
    "esp32c5": "ESP32-C5",
    "esp32c6": "ESP32-C6",
}


def parse_flash_args(path):
    """Yield (offset, relative path) for each part, in flashing order."""
    parts = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            # The first line is the flash mode/size/frequency options.
            if not line or line.startswith("--"):
                continue
            offset, _, name = line.partition(" ")
            if not name:
                continue
            parts.append((int(offset, 16), name.strip()))
    if not parts:
        raise SystemExit("no parts found in %s" % path)
    return sorted(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--board", required=True,
                    help="board profile name; artifacts are named after it, "
                         "because two boards can share a chip and need "
                         "different firmware")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--out", default="dist")
    args = ap.parse_args()

    family = CHIP_FAMILY.get(args.target)
    if family is None:
        raise SystemExit("unknown target %r -- add it to CHIP_FAMILY" % args.target)

    os.makedirs(args.out, exist_ok=True)
    parts = []
    for offset, rel in parse_flash_args(os.path.join(args.build_dir, "flash_args")):
        src = os.path.join(args.build_dir, rel)
        if not os.path.isfile(src):
            raise SystemExit("flash_args names %s, which the build did not produce" % src)
        # Per-board names: every board contributes a bootloader.bin and the
        # release holds them all in one flat namespace.
        stem, ext = os.path.splitext(os.path.basename(rel))
        name = "%s-%s%s" % (stem, args.board, ext)
        shutil.copyfile(src, os.path.join(args.out, name))
        parts.append({"path": name, "offset": offset})

    # The board travels with the build. ESP Web Tools matches on chipFamily
    # alone, which cannot tell a XIAO ESP32-C5 from a Waveshare one -- they are
    # the same chip and need different firmware -- so the page offers a manifest
    # per board and the chip check stays as a second line of defence.
    build = {"chipFamily": family, "board": args.board, "parts": parts}
    out = os.path.join(args.out, "builds-%s.json" % args.board)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(build, fh, indent=2)
        fh.write("\n")

    print("%s (%s) -> %s" % (args.board, args.target, out))
    json.dump(build, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
