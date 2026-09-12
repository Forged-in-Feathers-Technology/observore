#!/usr/bin/env python3
"""Fail when the documentation describes a release that the build no longer produces.

This exists because of a specific, repeated failure. Documentation states facts
that are true when written and are then quietly invalidated by a change
somewhere else: renaming the release assets broke the manual flashing
instructions in two documents, and nothing noticed until someone went looking.
The same shape has bitten this project more than once -- a scrubbed password
that was real, a regex still matching a pre-rename symbol.

The common thread is that prose has no compiler. These checks give the
checkable parts of it one. They are deliberately hermetic: no network, nothing
that can fail for reasons unrelated to the commit being tested.

Three checks:

  filenames  every firmware file named in the docs is one a supported target
             actually produces.  Catches the bare "bootloader.bin" that stopped
             existing when assets gained a target suffix.

  offsets    every documented offset/filename pair matches what this build's
             flash_args says.  Needs --build-dir, so it runs inside the
             per-target firmware job.  Catches the trap where someone adapts
             the S3 command for another chip and keeps 0x0 -- the C5 bootloader
             lives at 0x2000 and the board would never boot.

  targets    every target with an sdkconfig.defaults.<target> is in the CI
             matrix.  Catches adding a target and forgetting to build it.
"""

import argparse
import glob
import os
import re
import sys

DOCS = ["README.md", "web/index.html"]
STEMS = ("bootloader", "partition-table", "observore")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def known_targets():
    """Targets the repository claims to support, from their defaults files."""
    found = set()
    for path in glob.glob("sdkconfig.defaults.*"):
        suffix = path.split("sdkconfig.defaults.", 1)[1]
        if suffix and "." not in suffix:
            found.add(suffix)
    return found


def documented_files(text):
    """Firmware .bin filenames mentioned in a document."""
    return set(re.findall(r"\b((?:%s)[A-Za-z0-9_.-]*\.bin)\b" % "|".join(STEMS), text))


def documented_pairs(text):
    """(offset, filename) pairs written next to each other in a flash command."""
    pattern = r"(0x[0-9a-fA-F]+)\s+((?:%s)[A-Za-z0-9_.-]*\.bin)\b" % "|".join(STEMS)
    return [(int(o, 16), f) for o, f in re.findall(pattern, text)]


def parse_flash_args(build_dir):
    """{filename: offset} for what this build actually produces."""
    out = {}
    with open(os.path.join(build_dir, "flash_args"), encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("--"):
                continue
            offset, _, rel = line.partition(" ")
            if rel:
                out[os.path.basename(rel.strip())] = int(offset, 16)
    return out


def check_filenames(targets, problems):
    """Every documented firmware filename must belong to a supported target."""
    allowed = {"%s-%s.bin" % (stem, t) for stem in STEMS for t in targets}
    for doc in DOCS:
        for name in sorted(documented_files(read(doc))):
            if name not in allowed:
                problems.append(
                    "%s names %s, which no supported target produces "
                    "(expected <stem>-<target>.bin for one of: %s)"
                    % (doc, name, ", ".join(sorted(targets))))


def check_offsets(target, build_dir, problems):
    """Documented offsets for this target must match what the build emits."""
    actual = parse_flash_args(build_dir)
    actual = {"%s-%s.bin" % (os.path.splitext(k)[0], target): v
              for k, v in actual.items()}
    checked = matched = 0
    for doc in DOCS:
        for offset, name in documented_pairs(read(doc)):
            if not name.endswith("-%s.bin" % target):
                continue          # another target's command; its own job checks it
            checked += 1
            if name not in actual:
                problems.append("%s documents %s, which this build does not produce"
                                % (doc, name))
            elif actual[name] != offset:
                problems.append("%s flashes %s at 0x%x, but the build puts it at 0x%x"
                                % (doc, name, offset, actual[name]))
            else:
                matched += 1
    if not checked:
        problems.append("no documented flash offsets found for %s -- the docs "
                        "should show how to flash every supported target" % target)
    elif matched == checked:
        print("  %d documented offset(s) for %s match the build" % (matched, target))


def check_targets_are_built(targets, problems):
    """A target with defaults but no CI entry is a target nobody is testing."""
    ci = read(".github/workflows/ci.yml")
    matrix = re.search(r"target:\s*\[([^\]]+)\]", ci)
    if not matrix:
        problems.append(".github/workflows/ci.yml has no target matrix to check")
        return
    built = {t.strip() for t in matrix.group(1).split(",") if t.strip()}
    for t in sorted(targets - built):
        problems.append("sdkconfig.defaults.%s exists but %s is not in the CI "
                        "matrix, so nothing builds it" % (t, t))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", help="run the offset check for this target too")
    ap.add_argument("--build-dir", default="build")
    args = ap.parse_args()

    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

    targets = known_targets()
    if not targets:
        raise SystemExit("no sdkconfig.defaults.<target> files found")

    problems = []
    check_filenames(targets, problems)
    check_targets_are_built(targets, problems)
    if args.target:
        check_offsets(args.target, args.build_dir, problems)

    if problems:
        print("\ndocumentation no longer matches the build:\n", file=sys.stderr)
        for p in problems:
            print("  - %s" % p, file=sys.stderr)
        print("\nUpdate the documents above, or the build, so they agree.",
              file=sys.stderr)
        return 1

    print("ok: docs match the build (%s)" % ", ".join(sorted(targets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
