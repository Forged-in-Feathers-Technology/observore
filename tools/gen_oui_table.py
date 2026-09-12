#!/usr/bin/env python3
"""Generate main/observore_oui_table.h from the IEEE MA-L registry.

OUI assignments are public facts published by the IEEE Registration Authority.
Regenerating from the source keeps the table current and auditable instead of
carrying a hand-maintained copy that silently rots.

    python3 tools/gen_oui_table.py            # download, generate
    python3 tools/gen_oui_table.py --csv oui.csv   # use a local copy
"""

import argparse
import csv
import io
import re
import sys
import urllib.request
from collections import OrderedDict

OUI_URL = "https://standards-oui.ieee.org/oui/oui.csv"

# Each entry: (category, [regexes matched case-insensitively against the
# IEEE "Organization Name" column]).  Order matters -- the first category that
# claims an OUI owns it, so specific categories precede general ones.
CATEGORIES = OrderedDict([
    ("BODYCAM", [
        r"\baxon enterprise\b",
        r"\btaser international\b",
    ]),
    ("ALPR", [
        r"\bflock safety\b",
        r"\bneology\b",
        r"\bjenoptik\b",
        r"\bredflex\b",
        r"\btattile\b",          # Italian ALPR / traffic-enforcement cameras
    ]),
    ("FLEET_TELEMATICS", [
        r"\bsamsara networks\b",
        r"\blytx\b",
        r"\bnetradyne\b",
    ]),
    ("DRONE", [
        r"\bdji technology\b",
        r"\bdji baiwang\b",
        r"\bskydio\b",
        r"\bparrot sa\b",
    ]),
    ("CAMERA", [
        r"\bhikvision\b",
        r"\bdahua technolog",
        r"\buniview technolog",
        r"\bamcrest\b",
        r"\breolink\b",
        r"\bswann communications\b",
        r"\blorex technology\b",
        r"\baxis communications\b",
        r"\bmobotix\b",
        r"\bvivotek\b",
        r"\bhanwha vision\b",
        r"\bhanwha nxmd\b",
        r"\bhanwha vision vietnam\b",
        r"\bbosch security systems\b",
        r"\bavigilon\b",
        r"\bverkada\b",
        r"\bgenetec\b",
        r"\bring llc\b",
        r"\bwyze labs\b",
        r"\barlo technology\b",
        r"\bnest labs\b",
        r"\bsimplisafe\b",
    ]),
])

# MAC prefixes that are not IEEE assignments but are worth flagging anyway.
# Kept separate and explicit so the generated file stays reproducible.
EXTRA = [
    # (oui, category, label)
]

# Benign vendors, for LABELLING ONLY.  A match here never classifies a device,
# never scores, and never raises the threat level -- it only puts a name beside
# a MAC so you can tell your own phone from something you have never seen, and
# mute it in one click.
#
# Keeping this separate from CATEGORIES is the point: mixing "this is a
# surveillance camera" and "this is a Samsung" into one table is how a detector
# starts crying wolf at its owner's handset.
#
# Add vendors freely -- each prefix costs 4 bytes of flash.  Anything already
# claimed as a threat above keeps that claim.
VENDORS = OrderedDict([
    ("Apple",        [r"\bapple, inc"]),
    ("Samsung",      [r"\bsamsung\b"]),
    ("Google",       [r"\bgoogle\b"]),
    ("Huawei",       [r"\bhuawei\b"]),
    ("Xiaomi",       [r"\bxiaomi\b"]),
    ("Intel",        [r"\bintel corporate"]),
    ("Cisco",        [r"\bcisco systems"]),
    ("Espressif",    [r"\bespressif\b"]),
    ("Amazon",       [r"\bamazon tech"]),
    ("Microsoft",    [r"\bmicrosoft\b"]),
    ("Sony",         [r"\bsony\b"]),
    ("LG",           [r"\blg electronics\b"]),
    ("TP-Link",      [r"\btp-link\b"]),
    ("Texas Instr",  [r"texas instruments"]),
    ("HP",           [r"\bhewlett packard\b", r"\bhp inc\b"]),
    ("Dell",         [r"\bdell inc\b"]),
    ("Lenovo",       [r"\blenovo\b"]),
    ("Hon Hai",      [r"\bhon hai\b"]),
    ("Silicon Labs", [r"silicon lab"]),
    ("Nintendo",     [r"\bnintendo\b"]),
    ("ASUS",         [r"\basustek\b"]),
    ("AzureWave",    [r"\bazurewave\b"]),
    ("Murata",       [r"\bmurata\b"]),
    ("Netgear",      [r"\bnetgear\b"]),
    ("Liteon",       [r"\bliteon\b"]),
    ("Ubiquiti",     [r"\bubiquiti\b"]),
    ("Tuya",         [r"\btuya\b"]),
    ("Roku",         [r"\broku\b"]),
    ("Sonos",        [r"\bsonos\b"]),
    ("Belkin",       [r"\bbelkin\b"]),
    ("Bose",         [r"\bbose\b"]),
    ("Logitech",     [r"\blogitech\b"]),
    ("Garmin",       [r"garmin international"]),
    ("Fitbit",       [r"\bfitbit\b"]),
    ("Broadcom",     [r"\bbroadcom\b"]),
    ("Qualcomm",     [r"\bqualcomm\b"]),
    ("MediaTek",     [r"\bmediatek\b"]),
    ("Realtek",      [r"\brealtek\b"]),
    ("Nordic Semi",  [r"nordic semiconductor"]),
    ("Raspberry Pi", [r"raspberry pi"]),
    ("Philips",      [r"\bsignify\b", r"\bphilips lighting\b"]),
    ("IKEA",         [r"\bikea\b"]),
    ("Tesla",        [r"\btesla,? inc", r"tesla motors"]),
])


def fetch(path):
    if path:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    sys.stderr.write("downloading %s ...\n" % OUI_URL)
    req = urllib.request.Request(OUI_URL, headers={"User-Agent": "observore-oui-gen"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        return resp.read().decode("utf-8", errors="replace")


def normalise(org):
    """Collapse the registry's ragged vendor strings into a short label."""
    org = org.strip().strip('"').strip()
    org = re.sub(r"[,\.]?\s*(Co\.?|Corp\.?|Corporation|Inc\.?|Ltd\.?|Limited|"
                 r"LLC|GmbH|AB|SA|SRL|B\.?V\.?|Pty|S\.?A\.?S\.?|"
                 r"Technologies|Technology|Digital Technology)\b.*$", "", org,
                 flags=re.I)
    org = re.sub(r"\s+", " ", org).strip(" ,.-")
    return org[:23] or "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", help="local oui.csv instead of downloading")
    ap.add_argument("--out", default="main/observore_oui_table.h")
    args = ap.parse_args()

    compiled = OrderedDict(
        (cat, [re.compile(p, re.I) for p in pats])
        for cat, pats in CATEGORIES.items()
    )

    rows = []
    claimed = set()
    rows_raw = list(csv.DictReader(io.StringIO(fetch(args.csv))))
    for row in rows_raw:
        if row.get("Registry") != "MA-L":
            continue
        oui = (row.get("Assignment") or "").strip().upper()
        org = row.get("Organization Name") or ""
        if len(oui) != 6 or oui in claimed:
            continue
        for cat, pats in compiled.items():
            if any(p.search(org) for p in pats):
                rows.append((oui, cat, normalise(org)))
                claimed.add(oui)
                break

    for oui, cat, label in EXTRA:
        if oui not in claimed:
            rows.append((oui, cat, label))
            claimed.add(oui)

    # Benign vendors, second pass.  Threat claims above always win.
    vendor_names = list(VENDORS.keys())
    vendor_pats = [(i, [re.compile(p, re.I) for p in pats])
                   for i, pats in enumerate(VENDORS.values())]
    vendor_rows = []
    for row in rows_raw:
        if row.get("Registry") != "MA-L":
            continue
        oui = (row.get("Assignment") or "").strip().upper()
        org = row.get("Organization Name") or ""
        if len(oui) != 6 or oui in claimed:
            continue
        for idx, pats in vendor_pats:
            if any(p.search(org) for p in pats):
                vendor_rows.append((oui, idx))
                claimed.add(oui)
                break
    vendor_rows.sort(key=lambda r: r[0])

    rows.sort(key=lambda r: r[0])

    counts = OrderedDict((c, 0) for c in CATEGORIES)
    for _, cat, _ in rows:
        counts[cat] += 1

    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("/* Generated by tools/gen_oui_table.py -- do not edit by hand.\n"
                 " *\n"
                 " * Source: IEEE MA-L registry, %s\n"
                 " * Vendor-to-category mapping lives in the generator.\n"
                 " */\n\n"
                 "#pragma once\n\n"
                 "#include \"observore_types.h\"\n\n" % OUI_URL)
        fh.write("/* %d prefixes: %s */\n" % (
            len(rows), ", ".join("%s=%d" % (c, n) for c, n in counts.items())))
        fh.write("static const observore_oui_t OBSERVORE_OUI_TABLE[] = {\n")
        for oui, cat, label in rows:
            fh.write("    {{0x%s, 0x%s, 0x%s}, OBSERVORE_CLASS_%s, \"%s\"},\n"
                     % (oui[0:2], oui[2:4], oui[4:6], cat, label))
        fh.write("};\n\n")
        fh.write("#define OBSERVORE_OUI_TABLE_LEN "
                 "(sizeof(OBSERVORE_OUI_TABLE) / sizeof(OBSERVORE_OUI_TABLE[0]))\n")

        fh.write("\n/* Benign vendors -- LABELLING ONLY.  A match here never\n"
                 " * classifies, never scores, and never raises the threat\n"
                 " * level.  It exists so an unknown MAC can be recognised as\n"
                 " * your own handset and muted in one click.\n"
                 " */\n")
        fh.write("static const char *const OBSERVORE_VENDOR_NAMES[] = {\n")
        for name in vendor_names:
            fh.write('    "%s",\n' % name)
        fh.write("};\n\n")
        fh.write("#define OBSERVORE_VENDOR_NAMES_LEN "
                 "(sizeof(OBSERVORE_VENDOR_NAMES) / sizeof(OBSERVORE_VENDOR_NAMES[0]))"
                 "\n\n")
        fh.write("/* %d prefixes, 4 bytes each (%.1f KB of flash). */\n"
                 % (len(vendor_rows), len(vendor_rows) * 4 / 1024))
        fh.write("static const observore_vendor_oui_t OBSERVORE_VENDOR_OUIS[] = {\n")
        for oui, idx in vendor_rows:
            fh.write("    {{0x%s, 0x%s, 0x%s}, %d},\n"
                     % (oui[0:2], oui[2:4], oui[4:6], idx))
        fh.write("};\n\n")
        fh.write("#define OBSERVORE_VENDOR_OUIS_LEN "
                 "(sizeof(OBSERVORE_VENDOR_OUIS) / sizeof(OBSERVORE_VENDOR_OUIS[0]))\n")

    sys.stderr.write("wrote %s: %d threat prefixes (%s)\n" % (
        args.out, len(rows),
        ", ".join("%s=%d" % (c, n) for c, n in counts.items())))
    sys.stderr.write("  plus %d benign vendor prefixes across %d vendors "
                     "(%.1f KB)\n"
                     % (len(vendor_rows), len(vendor_names),
                        len(vendor_rows) * 4 / 1024))


if __name__ == "__main__":
    main()
