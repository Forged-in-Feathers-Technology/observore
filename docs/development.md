# Working on Observore

Checks to run before committing, the host test suite, and the generated tables.

## Tests

The classification, parsing and scoring logic builds and runs on the host with
no hardware and no ESP-IDF:

```bash
make -C test test
```

CI runs these on every push, alongside the credential scan, an ESP-IDF build
for every supported target, a check that the documents and the flasher page
still describe the release the build actually produces, a check that the links
between the documents resolve, a check that every persisted setting is in the
registry the namespace migration is generated from, a check that the console's
timezone table still matches tzdata, a parse of the console page's own script,
and a validation of the generated OUI table —
that both tables are sorted for binary search, carry no duplicates, stay
disjoint from each other, and index only names that exist.

That check deliberately does not re-download the IEEE registry to diff against.
The registry is unreachable from GitHub's runners, and it publishes new
assignments constantly, so such a job would fail for reasons unrelated to
whether this repository is correct. Refreshing the table stays a deliberate
manual step.

## Regenerating the OUI table

`main/observore_oui_table.h` is generated, not hand-maintained. Vendors get new
prefixes; refresh it with:

```bash
python3 tools/gen_oui_table.py
```

Both tables live in that one generated header. The threat categories are in
`CATEGORIES` and the benign vendor list is in `VENDORS` — add vendors there,
not to the generated header. A prefix claimed as a threat is never also listed
as benign.

Each benign prefix costs 4 bytes of flash (the name is an index into a shared
table, not a pointer per row). The 43 vendors shipped cost about 40 KB of
flash. Adding more is cheap; trimming `VENDORS` is the way to claw it back.
`idf.py build` reports how much of the app partition remains.

## Keeping credentials out of the repo

`CONFIG_OBSERVORE_WIFI_SSID` and `CONFIG_OBSERVORE_WIFI_PASSWORD` are empty in the
tracked `sdkconfig.defaults` and must stay that way. The easy mistake is to set
one with menuconfig, then paste it into `sdkconfig.defaults` to make it stick —
which is how a home network password ends up in a public repository.

```bash
tools/check_no_secrets.sh            # scan tracked files
tools/check_no_secrets.sh --staged   # scan what is about to be committed
```

Wire it up as a pre-commit hook if you intend to push this anywhere:

```bash
printf '#!/bin/sh\nexec tools/check_no_secrets.sh --staged\n' \
  > .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
```

The password is write-only from outside the device: no API returns it, and the
console never receives it, so the field stays blank even when a network is
configured.

## Nothing left behind

The other local check guards a subtler thing than a leaked password: a commit
that left part of itself behind.

```bash
tools/check_nothing_left_behind.sh            # list what is uncommitted
tools/check_nothing_left_behind.sh --strict   # and fail
```

```bash
printf '#!/bin/sh\nexec tools/check_nothing_left_behind.sh --strict\n' \
  > .git/hooks/pre-push && chmod +x .git/hooks/pre-push
```

It exists because of one commit here. A feature was staged as `git add main
boards README.md`, its test file was not on that list, and the feature reached
`main` with none of its coverage. CI passed: it ran the old tests, they were
green, and the checks that would have proved the new code works were never
there to run. **A green tick reported the absence of a test as the success of
one**, which is the most expensive kind of wrong a build can be.

No amount of care fixes that, because the habit causing it — naming paths to
`git add` — is fast precisely by being unexamined. So the check is mechanical:
after a commit, is anything still uncommitted that looks like part of it? It
deliberately ignores build output and scratch files, and deliberately notices
an untracked file under `main/`, `test/`, `tools/` or `boards/` — a new module
written, committed nowhere, and building fine locally because the file is on
disk.

It cannot run in CI, which is the point: CI only ever sees what was pushed,
and this is a check about what was not.
