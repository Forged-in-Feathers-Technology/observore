#!/usr/bin/env python3
"""Fail when a persisted setting is not in the one registry that describes them.

main/observore_nvs.h holds every NVS key as an X-macro, and the namespace
migration is generated from it. That was built because the list had previously
been written out in four places with nothing linking them, and a setting that
missed one of them was lost at the next rename.

Deriving the migration from one list was necessary and was not sufficient:
nothing checked that a setting reached the list at all, and five did not --
"bright", "touchcal", "census", "monitors" and "meshkey". The registry built to
survive a namespace change had stopped describing the device, and the failure
was invisible because everything works until the rename that erases them.

Two checks:

  declared  every key passed to observore_nvs_read/write appears in the
            registry. This is the one that had already failed five times.

  types     a key's type at every use site matches its declared type. A key
            declared STR and written as a BLOB migrates through the wrong
            nvs_get_* call, so the copy fails and the original is erased
            anyway -- the same loss by a different route.

Deliberately textual: it reads the sources rather than building them, so it
costs nothing and runs before a toolchain exists.
"""

import glob
import os
import re
import sys

HEADER = os.path.join("main", "observore_nvs.h")
SOURCES = sorted(glob.glob(os.path.join("main", "*.c")))

# X(NAME, "key", OBSERVORE_NVS_TYPE)
DECL = re.compile(r"X\(\s*\w+\s*,\s*\"([a-z0-9_]+)\"\s*,\s*(OBSERVORE_NVS_\w+)\s*\)")

# .key = "x" ... .type = OBSERVORE_NVS_Y, in either order and across lines.
# Designated initialisers are matched inside one brace-delimited initialiser
# rather than by line, because the two fields are split across lines in about
# half the call sites.
USE_KEY = re.compile(r"\.key\s*=\s*\"([a-z0-9_]+)\"")
USE_TYPE = re.compile(r"\.type\s*=\s*(OBSERVORE_NVS_\w+)")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def declared():
    """{key: type} from the registry."""
    text = read(HEADER)
    start = text.index("#define OBSERVORE_NVS_KEYS(X)")
    end = text.index("#define OBSERVORE_NVS_NAMESPACE")
    found = {}
    for key, type_ in DECL.findall(text[start:end]):
        found[key] = type_
    if not found:
        raise SystemExit("%s has no registry to read" % HEADER)
    return found


def uses():
    """[(file, line, key, type)] for every item initialiser in the sources."""
    out = []
    for src in SOURCES:
        text = read(src)
        for m in USE_KEY.finditer(text):
            key = m.group(1)
            line = text.count("\n", 0, m.start()) + 1
            # The type sits in the same initialiser: search from the key to the
            # end of the enclosing braces, and fall back to a short window when
            # the brace is not found (a one-line struct passed inline).
            close = text.find("}", m.end())
            window = text[m.end():close if close != -1 else m.end() + 200]
            # ...and it may precede the key instead.
            before = text.rfind("{", 0, m.start())
            if before != -1:
                window += text[before:m.start()]
            t = USE_TYPE.search(window)
            out.append((src, line, key, t.group(1) if t else None))
    return out


def main():
    reg = declared()
    problems = []

    for src, line, key, type_ in uses():
        if key not in reg:
            problems.append(
                "%s:%d writes \"%s\" through observore_nvs, but it is not in "
                "OBSERVORE_NVS_KEYS in %s -- a namespace change would not "
                "carry it across, and migrate() erases what it copied from"
                % (src, line, key, HEADER))
        elif type_ is None:
            problems.append(
                "%s:%d names \"%s\" with no .type beside it, so this check "
                "cannot tell whether it agrees with the registry"
                % (src, line, key))
        elif type_ != reg[key]:
            problems.append(
                "%s:%d uses \"%s\" as %s, but the registry declares it %s"
                % (src, line, key, type_, reg[key]))

    if problems:
        print("\nthe NVS registry does not describe the device:\n")
        for p in problems:
            print("  - %s" % p)
        print("\nAdd the key to OBSERVORE_NVS_KEYS in %s, with the type it is "
              "actually stored as.\n" % HEADER)
        return 1

    n = len(set(k for _, _, k, _ in uses()))
    print("ok: %d registered NVS key(s), %d use site(s), types agree"
          % (len(reg), len(uses())))
    if n != len(reg):
        print("   (%d of %d registered keys are not written anywhere, which is "
              "fine -- a retired key still has to migrate)" % (len(reg) - n, len(reg)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
