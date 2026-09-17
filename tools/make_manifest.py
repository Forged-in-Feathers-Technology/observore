#!/usr/bin/env python3
"""Assemble one ESP Web Tools manifest per board.

ESP Web Tools picks a build by matching chipFamily against the chip it finds on
the cable. That is enough to tell an ESP32-S3 from an ESP32-C5 and not enough
to tell two ESP32-C5 boards apart -- and they are not interchangeable: the
Waveshare kit has an addressable pixel and a UART bridge where the XIAO has a
plain LED on the same pin and only native USB. Installing the wrong one leaves
the LED dark and the console talking to hardware that is not there.

So each board gets its own manifest and the page offers a picker. The chipFamily
check still runs, which makes it a second line of defence: choosing a C6 profile
and plugging in a C5 is refused by the tool rather than flashed.

An index file lists the boards so the page can build its picker without having
the list hardcoded in two places.

Two callers want the same manifests with different paths: the release attaches
them next to the binaries (absolute URLs), and the Pages site serves the same
bytes same-origin (relative). Hence --base.
"""

import argparse
import glob
import json
import os

# What a human calls each board. Kept here rather than in the page so the list
# has one home, and the page builds its picker from boards.json.
LABELS = {
    "xiao-esp32s3":     "Seeed XIAO ESP32S3",
    "xiao-esp32c5":     "Seeed XIAO ESP32-C5",
    "xiao-esp32c6":     "Seeed XIAO ESP32C6",
    "devkit-esp32c5":   "ESP32-C5-DevKitC-1 / Waveshare C5",
    "cyd-2432s028r-st7789": "ESP32-2432S028R 2.8\" CYD (ST7789 panel)",
}

NOTES = {
    "xiao-esp32c6": "no PSRAM, so TLS for notifications is tight on this board",
    "devkit-esp32c5": "two USB sockets; either one works",
    "cyd-2432s028r-st7789": "no notifier in this build; ST7789 revision only, the screen is not driven yet",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--base", default="",
                    help="prefix for part paths; empty means same-origin relative")
    ap.add_argument("--out-dir", required=True,
                    help="directory to write manifest-<board>.json and boards.json into")
    ap.add_argument("fragments", nargs="+")
    args = ap.parse_args()

    paths = []
    for pattern in args.fragments:
        paths.extend(sorted(glob.glob(pattern)))
    if not paths:
        raise SystemExit("no build fragments matched %s" % args.fragments)

    index = []
    for path in paths:
        with open(path, encoding="utf-8") as fh:
            build = json.load(fh)
        board = build.pop("board", None)
        if not board:
            raise SystemExit(
                "%s has no board name.\n\n"
                "It was produced before the release started building per board "
                "rather than per chip, so it cannot say which of two boards "
                "sharing a chip it is for. This happens when the Pages site is "
                "rebuilt against a release older than that change: publish a "
                "newer release, or run the workflow against one." % path)
        if args.base:
            for part in build["parts"]:
                part["path"] = "%s/%s" % (args.base.rstrip("/"), part["path"])

        manifest = {
            # Exactly what the firmware answers to an Improv device-info request,
            # because that is how ESP Web Tools decides whether it is looking at
            # an upgrade or a new install: install-dialog.js compares the two
            # strings for equality and nothing else. With the board label in
            # here they never matched, every install of Observore over Observore
            # was treated as new, and -- see below -- was erased without asking.
            # The board is named in boards.json, which is what the picker reads.
            "name": "Observore",
            "version": args.version,
            # True means "ask", and the checkbox it produces defaults to off.
            # False does not mean "do not erase": it means erase a new install
            # without offering a choice. This was False, on the belief that it
            # was the safe setting, and the flasher wiped the mute rules, the
            # Wi-Fi credentials, the notifier token, the history and the console
            # password on every install. Read from the installer's source rather
            # than from its name.
            "new_install_prompt_erase": True,
            "builds": [build],
        }
        out = os.path.join(args.out_dir, "manifest-%s.json" % board)
        with open(out, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh, indent=2)
            fh.write("\n")
        index.append({"board": board,
                      "label": LABELS.get(board, board),
                      "chipFamily": build["chipFamily"],
                      "manifest": "manifest-%s.json" % board,
                      "note": NOTES.get(board, "")})
        print("  %-18s %-10s -> %s" % (board, build["chipFamily"], out))

    if not index:
        raise SystemExit("no boards produced a manifest")
    # Reference board first, so the default selection is the one most people
    # have. Getting it wrong is not dangerous -- ESP Web Tools refuses a chip
    # that does not match -- but being refused on the first click is a poor way
    # to meet a tool.
    order = {"xiao-esp32s3": 0, "devkit-esp32c5": 1, "xiao-esp32c5": 2,
             "xiao-esp32c6": 3, "cyd-2432s028r-st7789": 4}
    index.sort(key=lambda e: (order.get(e["board"], 99), e["label"]))
    idx = os.path.join(args.out_dir, "boards.json")
    with open(idx, "w", encoding="utf-8") as fh:
        json.dump({"version": args.version, "boards": index}, fh, indent=2)
        fh.write("\n")
    print("wrote %s listing %d boards" % (idx, len(index)))


if __name__ == "__main__":
    main()
