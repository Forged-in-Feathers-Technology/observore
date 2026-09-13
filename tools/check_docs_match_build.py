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
             matrix, and every target the release ships is built by CI.
             Catches adding a target and forgetting to build it.

"Supported" means two different things here and conflating them is a bug this
script has already made once.  A target can be *built* by CI without being
*shipped*: the C6 is compile-tested so portability breaks surface early, but no
C6 release asset exists, so documenting how to flash one would be wrong rather
than missing.  Filenames and offsets are therefore checked against the targets
in release.yml, not the ones in ci.yml.
"""

import argparse
import glob
import os
import re
import sys

DOCS = ["README.md", "web/index.html"]
# Every part a release carries. ota_data_initial joined the list when the
# partition table became OTA-shaped; leaving it out would have meant the one
# genuinely new offset in that change was the one nothing checked.
STEMS = ("bootloader", "partition-table", "observore", "ota_data_initial")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def known_targets():
    """Targets the repository can build, from their defaults files."""
    found = set()
    for path in glob.glob("sdkconfig.defaults.*"):
        suffix = path.split("sdkconfig.defaults.", 1)[1]
        if suffix and "." not in suffix:
            found.add(suffix)
    return found


def matrix_targets(workflow):
    """Every target named in a workflow matrix, or None when it has none.

    Handles both shapes in use: `target: [a, b]` for the portability matrix,
    and a repeated `target: a` inside an `include:` list for the board matrix.
    """
    text = read(workflow)
    listed = re.search(r"target:\s*\[([^\]]+)\]", text)
    found = set()
    if listed:
        found |= {t.strip() for t in listed.group(1).split(",") if t.strip()}
    found |= {m.strip() for m in re.findall(r"^\s*-?\s*target:\s*([A-Za-z0-9_]+)\s*$",
                                            text, flags=re.M)}
    return found or None


def released_boards():
    """Boards whose binaries a release actually carries.

    Boards, not chips. Two boards can share a chip and need different
    firmware -- a XIAO ESP32-C5 and a Waveshare one differ in LED type and in
    whether a UART bridge exists -- so the release names its artifacts after
    the board and the documentation has to match that.
    """
    text = read(".github/workflows/release.yml")
    found = {m.strip() for m in re.findall(r"^\s*-\s*board:\s*([A-Za-z0-9_.-]+)\s*$",
                                           text, flags=re.M)}
    if not found:
        raise SystemExit("release.yml has no board matrix to read")
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


def check_filenames(boards, problems):
    """Every documented firmware filename must be one a release actually carries."""
    allowed = {"%s-%s.bin" % (stem, b) for stem in STEMS for b in boards}
    for doc in DOCS:
        for name in sorted(documented_files(read(doc))):
            if name not in allowed:
                problems.append(
                    "%s names %s, which no released board produces "
                    "(expected <stem>-<board>.bin for one of: %s)"
                    % (doc, name, ", ".join(sorted(boards))))


def check_offsets(board, build_dir, problems):
    """Documented offsets for this board must match what the build emits."""
    actual = parse_flash_args(build_dir)
    actual = {"%s-%s.bin" % (os.path.splitext(k)[0], board): v
              for k, v in actual.items()}
    checked = matched = 0
    for doc in DOCS:
        for offset, name in documented_pairs(read(doc)):
            if not name.endswith("-%s.bin" % board):
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
        problems.append("no documented flash offsets found for %s, which the "
                        "release ships -- the docs should show how to flash "
                        "every board a user can download" % board)
    elif matched == checked:
        print("  %d documented offset(s) for %s match the build" % (matched, board))


def check_page_firmware_refs(boards, problems):
    """Every firmware/<file> the page names must be one a release produces.

    The filename check above looks at documented flash commands. This looks at
    what the page fetches, which is a different surface and broke separately:
    the version line went on requesting firmware/manifest.json after the
    release started shipping a manifest per board, so it reported a failure
    while everything around it worked. Nothing caught that, because a JavaScript
    fetch is not a documented command.
    """
    allowed = {"boards.json"}
    allowed |= {"manifest-%s.json" % b for b in boards}
    page = read("web/index.html")
    for ref in sorted(set(re.findall(r"firmware/([A-Za-z0-9_.-]+)", page))):
        if ref not in allowed:
            problems.append(
                "web/index.html fetches firmware/%s, which no release "
                "produces (expected boards.json or manifest-<board>.json for "
                "one of: %s)" % (ref, ", ".join(sorted(boards))))


def check_targets_are_built(targets, problems):
    """A target nobody builds is a target nobody is testing."""
    built = matrix_targets(".github/workflows/ci.yml")
    if built is None:
        problems.append(".github/workflows/ci.yml has no target matrix to check")
        return
    for t in sorted(targets - built):
        problems.append("sdkconfig.defaults.%s exists but %s is not in the CI "
                        "matrix, so nothing builds it" % (t, t))


def check_boards_are_built(shipped, problems):
    """A board the release ships must be one CI builds."""
    ci = read(".github/workflows/ci.yml")
    built = {m.strip() for m in re.findall(r"^\s*-\s*board:\s*([A-Za-z0-9_.-]+)\s*$",
                                           ci, flags=re.M)}
    for b in sorted(shipped - built):
        problems.append("release.yml ships the %s board but CI never builds it, "
                        "so a release is the first thing that would find a "
                        "break" % b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", help="run the offset check for this board too")
    ap.add_argument("--build-dir", default="build")
    args = ap.parse_args()

    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

    targets = known_targets()
    if not targets:
        raise SystemExit("no sdkconfig.defaults.<target> files found")

    shipped = released_boards()

    problems = []
    check_filenames(shipped, problems)
    check_page_firmware_refs(shipped, problems)
    check_targets_are_built(targets, problems)
    check_boards_are_built(shipped, problems)
    if args.board and args.board in shipped:
        check_offsets(args.board, args.build_dir, problems)
    elif args.board:
        print("  %s is not a released board; nothing to document" % args.board)

    if problems:
        print("\ndocumentation no longer matches the build:\n", file=sys.stderr)
        for p in problems:
            print("  - %s" % p, file=sys.stderr)
        print("\nUpdate the documents above, or the build, so they agree.",
              file=sys.stderr)
        return 1

    print("ok: docs match the build (chips: %s | boards shipped: %s)"
          % (", ".join(sorted(targets)), ", ".join(sorted(shipped))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
