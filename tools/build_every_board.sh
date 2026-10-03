#!/usr/bin/env bash
#
# Build every board in the CI matrix, locally, before pushing.
#
# This exists because of one failure repeated twice. Eight of sixteen CI builds
# were broken for ten commits because CONFIG_OBSERVORE_TZ sat inside
# "if OBSERVORE_DISPLAY"; then observore_display_taps() was called from code
# compiled without CONFIG_OBSERVORE_WATCHFACE. Both times the cause was the
# same: the board on the desk built, so the work looked done. A profile is a
# set of #ifdefs, and the ones you are not building are the ones that break.
#
# The matrix is read from release.yml rather than listed here, because a list
# kept in two places is a list that disagrees with itself. release.yml is the
# shipped set; CI builds that plus anything compile-tested, so a board that is
# built but not shipped needs adding to BUILD_ONLY below.
#
#   tools/build_every_board.sh              every board, stopping at the first failure
#   tools/build_every_board.sh -k           keep going, report at the end
#   tools/build_every_board.sh nm-cyd-c5    just these
#
set -uo pipefail
cd "$(dirname "$0")/.."

# Boards CI compiles but no release ships. Empty is the normal state; a board
# lands here while its hardware is unverified and leaves when it ships.
BUILD_ONLY=()

KEEP=0
[[ "${1:-}" == "-k" ]] && { KEEP=1; shift; }
WANT=("$@")

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "ESP-IDF is not on the path. Source export.sh first:" >&2
    echo "    . ~/esp/esp-idf/export.sh" >&2
    exit 2
fi

# One awk pass over the matrix: each "- board:" starts a record, and target and
# profile follow it. Reading the file beats restating it.
# The workflow names "target:" twice: once per matrix row, and again in the
# step that consumes it as ${{ matrix.target }}. Reading the whole file gave
# every board that literal string as its target, so the walk stops at "steps:".
mapfile -t ROWS < <(awk '
    /^[[:space:]]*steps:[[:space:]]*$/ { done = 1 }
    done { next }
    /^[[:space:]]*- board:[[:space:]]*/ {
        if (b != "") print b "\t" t "\t" p
        b = $3; t = ""; p = ""; next
    }
    /^[[:space:]]*target:[[:space:]]*/  { t = $2; next }
    /^[[:space:]]*profile:[[:space:]]*/ {
        p = $2; gsub(/"/, "", p); next
    }
    END { if (b != "") print b "\t" t "\t" p }
' .github/workflows/release.yml)

for b in "${BUILD_ONLY[@]}"; do
    ROWS+=("$b	$(grep -A2 "board: $b" .github/workflows/ci.yml | awk '/target:/{print $2; exit}')	boards/$b.defaults")
done

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
pass=(); fail=()

for row in "${ROWS[@]}"; do
    IFS=$'\t' read -r board target profile <<< "$row"
    [[ -z "$board" || -z "$target" ]] && continue
    if [[ ${#WANT[@]} -gt 0 ]]; then
        printf '%s\n' "${WANT[@]}" | grep -qx "$board" || continue
    fi

    defaults="sdkconfig.defaults"
    [[ -n "$profile" ]] && defaults="sdkconfig.defaults;$profile"

    printf '%-26s %-9s ' "$board" "$target"
    log="$OUT/$board.log"
    # --preview because the C5 is still behind it, and it is harmless elsewhere.
    # -DSDKCONFIG as well as -B: the build tree moves with -B, the resolved
    # sdkconfig does not. Without this every board writes the project root's
    # sdkconfig in turn, each one clobbering the last -- and the check below
    # would be reading whichever board finished most recently.
    if idf.py -B "$OUT/$board" -DSDKCONFIG="$OUT/$board/sdkconfig" \
              -DSDKCONFIG_DEFAULTS="$defaults" \
              --preview set-target "$target" build >"$log" 2>&1; then
        # The thing CI asserts: that the layering resolved to this board and
        # not to whatever the previous build left behind.
        got=$(sed -n 's/^CONFIG_OBSERVORE_BOARD="\(.*\)"$/\1/p' "$OUT/$board/sdkconfig")
        want="$board"
        if [[ "$got" != "$want" ]]; then
            echo "built, but resolved to '$got' rather than '$want'"
            fail+=("$board (wrong board name)")
        else
            size=$(stat -c%s "$OUT/$board/observore.bin" 2>/dev/null || echo 0)
            printf 'ok   %s KB\n' "$((size / 1024))"
            pass+=("$board")
        fi
    else
        echo "FAILED"
        grep -nE "error:|Error|undefined reference" "$log" | head -6 | sed 's/^/    /'
        echo "    full log: $log"
        fail+=("$board")
        if [[ $KEEP -eq 0 ]]; then
            echo
            echo "Stopped at the first failure. -k builds the rest anyway."
            # The log lives under a trap, so hand it over before it goes.
            cp "$log" "/tmp/observore-build-$board.log" 2>/dev/null &&
                echo "Log kept at /tmp/observore-build-$board.log"
            exit 1
        fi
    fi
done

echo
echo "${#pass[@]} built, ${#fail[@]} failed"
if [[ ${#fail[@]} -gt 0 ]]; then
    printf '  failed: %s\n' "${fail[*]}"
    exit 1
fi
