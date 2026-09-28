#!/usr/bin/env bash
# Fail when a commit left part of its own change behind.
#
# This exists because of one specific, embarrassing shape. A feature was
# committed with
#
#     git add main boards README.md
#
# and its test file was not on that list. The feature reached main with none
# of its coverage, and CI was perfectly happy: it ran the old tests, they
# passed, and the seven new checks that would have proved the change works
# were never there to run. A green tick reported the absence of a test as the
# success of one.
#
# Naming paths to `git add` is how it happens, and no amount of care fixes a
# habit -- the whole point of the habit is that it is fast and unexamined. So
# the check is mechanical: after committing, is anything still uncommitted
# that looks like part of what was just committed?
#
# Deliberately narrow. Untracked scratch files, build output and editor
# leavings are not the target; a modified source or test file sitting beside
# a commit that touched the same areas is.
#
#   tools/check_nothing_left_behind.sh         # warn about what is uncommitted
#   tools/check_nothing_left_behind.sh --strict  # and exit non-zero
set -euo pipefail

cd "$(dirname "$0")/.."

strict=0
[[ "${1:-}" == "--strict" ]] && strict=1

# Tracked files with changes that are not committed, staged or otherwise.
dirty=$(git status --porcelain --untracked-files=no | awk '{print $2}')

# Untracked files in the places source lives, which is the other half of the
# same mistake: a new module written, committed nowhere, and the build still
# passing locally because the file is on disk.
untracked=$(git ls-files --others --exclude-standard -- main test tools boards \
            | grep -vE '\.(o|d|bin|elf|map)$' || true)

left="$(printf '%s\n%s' "$dirty" "$untracked" | sed '/^$/d')"

if [[ -z "$left" ]]; then
    echo "nothing left behind"
    exit 0
fi

echo "Uncommitted work sits beside the last commit:"
echo "$left" | sed 's/^/  /'
echo
echo "If it belongs to what was just committed, it is missing from it."
echo "A test left out this way passes CI by not existing."

exit $(( strict ))
