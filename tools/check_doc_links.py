#!/usr/bin/env python3
"""Fail when a link between the documents points at nothing.

This exists because of the split. The README was a single 3,690-line file, and
a single file cannot have a broken internal link that a reader notices and the
author does not -- every anchor either resolved or was obviously absent on the
same screen. Moving the long write-ups into docs/ bought a readable front page
and took that property away: a heading renamed in docs/memory.md now silently
breaks a sentence in README.md, and nothing about either file looks wrong.

Markdown has no compiler, so the links get one. Two checks:

  targets   every relative link resolves to a file that exists, from the
            directory of the document doing the linking -- which is the part
            that breaks when a document moves rather than when it changes.

  anchors   every #fragment resolves to a heading in the document it names,
            using GitHub's slug rules.

Deliberately hermetic: no network, so an external URL is not checked here. A
dead external link is a different failure with a different lifetime, and a
check that reaches the network fails for reasons unrelated to the commit.
"""

import os
import re
import sys

# The documents that link to each other. Anything a reader navigates.
DOCS = ["README.md"] + sorted(
    os.path.join("docs", f) for f in os.listdir("docs") if f.endswith(".md")
)

# [text](target) -- target captured up to the closing paren. Markdown link
# text can itself wrap across lines, so the whole file is searched as one
# string rather than line by line.
LINK = re.compile(r"\[(?:[^\]]*)\]\(([^)\s]+)\)")

# Fenced code blocks are examples, not navigation. A shell snippet containing
# a bracketed expression followed by a paren is not a link and has produced a
# false failure in every naive version of this check.
FENCE = re.compile(r"^```", re.M)


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def strip_code(text):
    """Drop fenced blocks, keeping line count so line numbers stay true."""
    out, inside = [], False
    for line in text.split("\n"):
        if line.startswith("```"):
            inside = not inside
            out.append("")
            continue
        out.append("" if inside else line)
    return "\n".join(out)


def slug(heading):
    """GitHub's anchor for a heading: lowercase, punctuation dropped, spaces to dashes."""
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s, flags=re.UNICODE)
    return s.replace(" ", "-")


def anchors(path):
    """Every anchor a document offers, from its headings."""
    found = set()
    for line in strip_code(read(path)).split("\n"):
        m = re.match(r"^#{1,6}\s+(.*)", line)
        if m:
            found.add(slug(m.group(1)))
    return found


def links(path):
    """(line number, target) for every relative link in a document."""
    text = strip_code(read(path))
    out = []
    for m in LINK.finditer(text):
        target = m.group(1)
        if re.match(r"^(https?:|mailto:|tel:)", target):
            continue
        out.append((text.count("\n", 0, m.start()) + 1, target))
    return out


def main():
    problems = []
    cache = {}

    for doc in DOCS:
        here = os.path.dirname(doc)
        for lineno, target in links(doc):
            path, _, frag = target.partition("#")

            if not path:                      # same-document anchor
                dest = doc
            else:
                dest = os.path.normpath(os.path.join(here, path))
                if not os.path.exists(dest):
                    problems.append("%s:%d links to %s, which does not exist"
                                    % (doc, lineno, target))
                    continue

            if not frag:
                continue
            if not dest.endswith(".md"):
                continue                      # only Markdown offers headings
            if dest not in cache:
                cache[dest] = anchors(dest)
            if frag not in cache[dest]:
                problems.append("%s:%d links to #%s in %s, which has no such heading"
                                % (doc, lineno, frag, dest if path else "itself"))

    if problems:
        print("\nbroken links between the documents:\n")
        for p in problems:
            print("  - %s" % p)
        print("\nA heading was renamed or a file moved. Fix the link or restore "
              "the heading.\n")
        return 1

    total = sum(len(links(d)) for d in DOCS)
    print("ok: %d relative link(s) across %d document(s) resolve"
          % (total, len(DOCS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
