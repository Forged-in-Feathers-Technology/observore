#!/usr/bin/env bash
#
# Refuse committed diagnostic scaffolding.
#
# This exists because of a specific failure. Diagnosing a blank screen, I
# added two things to the display: a line forcing the stored brightness to
# full so the panel would be visibly lit, and a loop driving the backlight
# hard on and hard off every three seconds so the board's own photoresistor
# could say whether the backlight worked at all. Both were marked TEMP. Both
# were committed, pushed, and passed CI -- because they compile, and because
# the only guard watching for leftovers (check_nothing_left_behind.sh) looks
# for work sitting *beside* a commit rather than inside one.
#
# The user found it the way users find this sort of thing: their screen was
# flashing, and asked whether that was expected. It was my debug loop doing
# exactly what I had written it to do. Worse, the forced brightness line
# silently disabled a real feature -- the stored level was ignored on every
# board.
#
# So the markers themselves are now the check. Anything that announces itself
# as temporary has no business in a commit, and a marker is cheap to write and
# cheap to find. This does not catch unmarked scaffolding; it catches the
# scaffolding people actually leave, which is the kind they meant to remove.
set -uo pipefail
cd "$(dirname "$0")/.."

# Only the sources and the console page. Documentation may legitimately
# discuss a temporary file, and this script has to be able to name the
# markers it looks for.
FILES=$(git ls-files 'main/*.c' 'main/*.h' 'main/www/*.html' 'tools/*.py' 2>/dev/null)
[[ -z "$FILES" ]] && { echo "no source files found"; exit 0; }

# Markers that mean "I was going to take this out again". XXX and FIXME are
# deliberately absent: those mark real work that is allowed to be committed
# and discussed, and conflating the two would make this noise.
PATTERN='\b(TEMP|TEMPORARY|HACK|DEBUG ONLY|REMOVE ME|DO NOT COMMIT)\b'

# shellcheck disable=SC2086
hits=$(grep -nIE "$PATTERN" $FILES 2>/dev/null | grep -v 'check_no_scaffolding' || true)

if [[ -n "$hits" ]]; then
    echo "Diagnostic scaffolding left in the source:" >&2
    echo "$hits" | sed 's/^/  /' >&2
    echo >&2
    echo "These markers mean the code was meant to come out again. If one is" >&2
    echo "a false positive -- a comment about temperature, say -- reword it;" >&2
    echo "the marker is the check." >&2
    exit 1
fi
echo "no scaffolding left in the source"
