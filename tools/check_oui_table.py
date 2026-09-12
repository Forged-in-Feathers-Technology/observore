#!/usr/bin/env python3
"""Validate the generated OUI header against the invariants the firmware relies on.

This deliberately does NOT download the IEEE registry.  A check that reaches
out to a third party fails when that third party is unreachable or has simply
published something new, neither of which means this repository is wrong -- and
the registry is in fact unreachable from GitHub's runners.  Regenerating is a
deliberate, manual act (`python3 tools/gen_oui_table.py`); this only confirms
that whatever was committed is internally consistent.

What it checks, and why each one matters:

  * both tables sorted ascending, no duplicate prefixes -- observore_oui_lookup()
    and observore_vendor_lookup() are binary searches, so an unsorted or
    duplicated table silently returns the wrong vendor, or misses.
  * no prefix in both tables -- a benign vendor match must never be able to
    classify, and a threat prefix must never be reported as ordinary kit.
  * every vendor index within the names array -- an out-of-range index would
    read past the end of ARGUS_VENDOR_NAMES at runtime.
  * the counts in the generated comments match the rows actually present --
    catches a hand-edited or half-regenerated header.
"""

import re
import sys

HEADER = "main/observore_oui_table.h"


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    return 1


def main():
    src = open(HEADER, encoding="utf-8").read()
    problems = 0

    names = re.search(r"ARGUS_VENDOR_NAMES\[\] = \{(.*?)\};", src, re.S)
    names = re.search(r"OBSERVORE_VENDOR_NAMES\[\] = \{(.*?)\};", src, re.S) or names
    if not names:
        return fail("vendor name array not found")
    vendor_names = re.findall(r'"(.*?)"', names.group(1))

    threat = re.search(r"OBSERVORE_OUI_TABLE\[\] = \{(.*?)\};", src, re.S)
    if not threat:
        return fail("threat table not found")
    threat_rows = re.findall(
        r"\{\{0x(..), 0x(..), 0x(..)\}, OBSERVORE_CLASS_(\w+), \"(.*?)\"\}",
        threat.group(1))

    vendor = re.search(r"OBSERVORE_VENDOR_OUIS\[\] = \{(.*?)\};", src, re.S)
    if not vendor:
        return fail("vendor table not found")
    vendor_rows = re.findall(r"\{\{0x(..), 0x(..), 0x(..)\}, (\d+)\}",
                             vendor.group(1))

    threat_ouis = [a + b + c for a, b, c, _, _ in threat_rows]
    vendor_ouis = [a + b + c for a, b, c, _ in vendor_rows]

    for label, ouis in (("threat", threat_ouis), ("vendor", vendor_ouis)):
        if ouis != sorted(ouis):
            problems |= fail(f"{label} table is not sorted; binary search "
                             f"needs it to be")
        dupes = {o for o in ouis if ouis.count(o) > 1} if len(ouis) < 2000 \
            else _dupes(ouis)
        if dupes:
            problems |= fail(f"{label} table has duplicate prefixes: "
                             f"{sorted(dupes)[:5]}")

    overlap = set(threat_ouis) & set(vendor_ouis)
    if overlap:
        problems |= fail("a prefix is claimed as both a threat and benign, so "
                         f"labelling could suppress a detection: {sorted(overlap)[:5]}")

    for _, _, _, idx in vendor_rows:
        if int(idx) >= len(vendor_names):
            problems |= fail(f"vendor index {idx} is past the end of the "
                             f"{len(vendor_names)}-entry name array")
            break

    declared = re.search(r"/\* (\d+) prefixes:", src)
    if declared and int(declared.group(1)) != len(threat_rows):
        problems |= fail(f"header claims {declared.group(1)} threat prefixes "
                         f"but carries {len(threat_rows)}")
    declared_v = re.search(r"/\* (\d+) prefixes, 4 bytes each", src)
    if declared_v and int(declared_v.group(1)) != len(vendor_rows):
        problems |= fail(f"header claims {declared_v.group(1)} vendor prefixes "
                         f"but carries {len(vendor_rows)}")

    if problems:
        return 1
    print(f"ok: {len(threat_rows)} threat prefixes, {len(vendor_rows)} vendor "
          f"prefixes across {len(vendor_names)} vendors, sorted, disjoint, "
          f"indices in range")
    return 0


def _dupes(items):
    seen, dupes = set(), set()
    for i in items:
        (dupes if i in seen else seen).add(i)
    return dupes


if __name__ == "__main__":
    sys.exit(main())
