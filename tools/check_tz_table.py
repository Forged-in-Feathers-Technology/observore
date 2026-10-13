#!/usr/bin/env python3
"""Verify the console's timezone table against the IANA database.

The console offers a list of zone names, because ESP-IDF ships no zone database
and "America/Toronto" means nothing to newlib -- so the page has to carry the
POSIX TZ string for each name it offers. Those strings encode daylight-saving
rules by hand, and a hand-written rule is a rule nobody has checked.

This checks them. For every (name, POSIX string) pair in the page it compares
the UTC offset every five days over two years against the IANA database, and
the POSIX side is evaluated by the C library rather than reinterpreted here --
because the C library is what the device will actually use. Reading my own
table back in Python would only prove I can read my own table.

It earned its place immediately. Written from memory, the table had
America/Vancouver as "PST8PDT,M3.2.0,M11.1.0" and America/Edmonton as
"MST7MDT,M3.2.0,M11.1.0", which are correct today and wrong from 1 November
2026: tzdata 2026c has British Columbia and Alberta adopting permanent
daylight saving, modelled as a change of standard zone to MST and CST. A POSIX
string has no year qualifiers and cannot express a rule that changes on a
date, so those two zones are not representable across that boundary at all and
are deliberately absent from the table. The fixed-offset fallback covers them.

A failure here means the shipped table disagrees with this machine's tzdata.
That is worth failing on: either a zone's rules changed and the table is now
wrong, or a zone has become unrepresentable and should be dropped.

  tools/check_tz_table.py            # verify what the page carries
  tools/check_tz_table.py --emit     # print a table of what verifies

Requires a C compiler and a tzdata, both of which CI has. No network.
"""

import datetime
import os
import re
import subprocess
import sys
import tempfile

try:
    from zoneinfo import ZoneInfo
except ImportError:                                   # pragma: no cover
    print("no zoneinfo module; cannot check the table")
    sys.exit(1)

PAGE = os.path.join("main", "www", "index.html")

# 'Area/City': 'POSIX', one per line, inside the marked block.
ENTRY = re.compile(r"'([A-Za-z_]+/[A-Za-z_/]+|UTC)':\s*'([^']+)'")
BEGIN = "/* TZ TABLE BEGIN"
END = "/* TZ TABLE END"

# Every five days over two years crosses every transition twice, including a
# zone that changes its rule partway through -- which is how the Canadian
# zones were caught.
SAMPLE_DAYS = 5
SPAN_DAYS = 730

HELPER = r"""
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv)
{
    if (argc < 3) { return 1; }
    setenv("TZ", argv[1], 1);
    tzset();
    for (int i = 2; i < argc; i++) {
        time_t t = (time_t)strtoll(argv[i], NULL, 10);
        struct tm lt, gt;
        localtime_r(&t, &lt);
        gmtime_r(&t, &gt);
        long off = (lt.tm_hour * 60 + lt.tm_min) - (gt.tm_hour * 60 + gt.tm_min);
        int dd = lt.tm_mday - gt.tm_mday;
        if (dd == 1 || dd < -1)       { off += 1440; }
        else if (dd == -1 || dd > 1)  { off -= 1440; }
        printf("%ld\n", off);
    }
    return 0;
}
"""


def table_from_page():
    with open(PAGE, encoding="utf-8") as fh:
        text = fh.read()
    try:
        block = text[text.index(BEGIN):text.index(END)]
    except ValueError:
        raise SystemExit("%s has no TZ TABLE block to check" % PAGE)
    pairs = ENTRY.findall(block)
    if not pairs:
        raise SystemExit("the TZ TABLE block in %s is empty" % PAGE)
    return pairs


def build_helper(workdir):
    src = os.path.join(workdir, "offs.c")
    exe = os.path.join(workdir, "offs")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(HELPER)
    subprocess.run(["cc", "-O1", "-o", exe, src], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return exe


def stamps():
    start = datetime.datetime.now(datetime.timezone.utc).replace(
        hour=12, minute=0, second=0, microsecond=0)
    n = SPAN_DAYS // SAMPLE_DAYS
    return [int((start + datetime.timedelta(days=SAMPLE_DAYS * i)).timestamp())
            for i in range(n)]


def check(pairs, exe, when):
    problems, ok = [], []
    args = [str(s) for s in when]
    for name, posix in pairs:
        try:
            zone = ZoneInfo(name)
        except Exception as exc:
            problems.append("%s is not a zone this tzdata knows (%s)" % (name, exc))
            continue
        out = subprocess.run([exe, posix] + args, capture_output=True, text=True)
        got = [int(x) for x in out.stdout.split()]
        if len(got) != len(when):
            problems.append("%s: the C library produced %d offsets of %d for %r"
                            % (name, len(got), len(when), posix))
            continue
        bad = None
        for stamp, mine in zip(when, got):
            theirs = int(datetime.datetime.fromtimestamp(stamp, zone)
                         .utcoffset().total_seconds() // 60)
            if theirs != mine:
                bad = ("%s: on %s the database says %+d minutes and %r gives "
                       "%+d" % (name,
                                datetime.datetime.fromtimestamp(
                                    stamp, datetime.timezone.utc).date(),
                                theirs, posix, mine))
                break
        if bad:
            problems.append(bad)
        else:
            ok.append((name, posix))
    return ok, problems


def main():
    emit = "--emit" in sys.argv
    pairs = table_from_page()
    when = stamps()
    with tempfile.TemporaryDirectory() as workdir:
        try:
            exe = build_helper(workdir)
        except subprocess.CalledProcessError as exc:
            print("could not build the offset helper:\n%s"
                  % exc.stderr.decode("utf-8", "replace"))
            return 1
        ok, problems = check(pairs, exe, when)

    if emit:
        for name, posix in ok:
            print("  '%s': '%s'," % (name, posix))
        return 0

    if problems:
        print("\nthe console's timezone table disagrees with this tzdata:\n")
        for p in problems:
            print("  - %s" % p)
        print("\nEither a zone's rules changed and its POSIX string is now "
              "wrong, or the zone has stopped being expressible as one POSIX "
              "string and should be dropped from the table -- the fixed-offset "
              "fallback covers anybody it leaves out.\n")
        return 1

    print("ok: %d zones in %s match this tzdata across %d days"
          % (len(ok), PAGE, SPAN_DAYS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
