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


def image_version(path):
    """Read the version out of a built application image.

    The esp_app_desc_t structure sits immediately after the image and first
    segment headers, at a fixed offset, and its version field is a 32-byte
    NUL-padded string. Read directly rather than shelled out to esptool so this
    stays a pure check with no toolchain in the way.
    """
    with open(path, "rb") as fh:
        head = fh.read(0x70)
    if len(head) < 0x70 or head[0] != 0xE9:
        raise SystemExit("%s is not an ESP application image" % path)
    return head[0x30:0x50].split(b"\x00")[0].decode("ascii", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--board", required=True,
                    help="board profile name; artifacts are named after it, "
                         "because two boards can share a chip and need "
                         "different firmware")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--out", default="dist")
    ap.add_argument("--expect-version",
                    help="tag this release is being cut as; the application "
                         "descriptor in the built image must match it")
    args = ap.parse_args()

    if args.expect_version:
        app = os.path.join(args.build_dir, "observore.bin")
        found = image_version(app)
        if found != args.expect_version:
            raise SystemExit(
                "the built image says %r but this release is %r.\n"
                "ESP-IDF takes the version from git describe unless version.txt\n"
                "exists, so a stray version.txt -- the kind written by hand to\n"
                "test an update -- silently stamps the whole release with it."
                % (found, args.expect_version))
        print("image version %s matches the tag" % found)

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
