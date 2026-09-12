#!/usr/bin/env python3
"""Assemble an ESP Web Tools manifest from per-target build fragments.

ESP Web Tools picks the build whose chipFamily matches the chip it finds on the
other end of the cable, so a manifest carrying every target is what makes the
flasher work without asking the user which chip they have.

Two callers want the same manifest with different paths: the release attaches
it next to the binaries on a GitHub release (absolute URLs), and the Pages site
serves the same bytes same-origin (relative). Hence --base.
"""

import argparse
import glob
import json


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--base", default="",
                    help="prefix for part paths; empty means same-origin relative")
    ap.add_argument("--out", required=True)
    ap.add_argument("fragments", nargs="+")
    args = ap.parse_args()

    paths = []
    for pattern in args.fragments:
        paths.extend(sorted(glob.glob(pattern)))
    if not paths:
        raise SystemExit("no build fragments matched %s" % args.fragments)

    builds = []
    for path in paths:
        with open(path, encoding="utf-8") as fh:
            build = json.load(fh)
        if args.base:
            for part in build["parts"]:
                part["path"] = "%s/%s" % (args.base.rstrip("/"), part["path"])
        builds.append(build)

    families = [b["chipFamily"] for b in builds]
    if len(set(families)) != len(families):
        raise SystemExit("two builds claim the same chipFamily: %s" % families)

    manifest = {
        "name": "Observore",
        "version": args.version,
        # An upgrade must not erase NVS: the mute rules, Wi-Fi credentials,
        # notifier token and generated console password all live there. ESP Web
        # Tools still offers an erase checkbox for a deliberate clean install.
        "new_install_prompt_erase": False,
        "builds": builds,
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")
    print("wrote %s for %s" % (args.out, ", ".join(families)))


if __name__ == "__main__":
    main()
