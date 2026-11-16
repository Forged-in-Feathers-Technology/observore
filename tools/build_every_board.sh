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
#   tools/build_every_board.sh --census [-k] [boards]
#                                           the same boards with boards/mesh-census.defaults
#                                           layered last
#
# The census exchange is off in every board profile, so nothing in the ordinary
# matrix compiles it, and code no build touches is code that rots. --census
# layers the option on and builds the same boards. A "depends on" that cannot be
# met drops a defaults line silently -- the classic ESP32 is meant to refuse the
# option, and a mistyped dependency would refuse it everywhere -- so a census
# build is judged by what resolved and what linked, never by the layer file:
# the S3, C5 and C6 must resolve CONFIG_OBSERVORE_MESH_CENSUS=y with encrypted NVS
# and link the radio path, and a classic ESP32 must resolve no such line and link
# none of it. An ordinary run proves the opposite for every board it builds: the
# exchange is absent unless somebody asked for it.
#
# One line is printed per board, in release.yml order whatever order the names
# were given in, with and without --census; that is how two runs are compared.
#
set -uo pipefail
cd "$(dirname "$0")/.."

# Boards CI compiles but no release ships. Empty is the normal state; a board
# lands here while its hardware is unverified and leaves when it ships.
BUILD_ONLY=()

KEEP=0
CENSUS=0
WANT=()
for arg in "$@"; do
    case "$arg" in
    -k)       KEEP=1 ;;
    --census) CENSUS=1 ;;
    *)        WANT+=("$arg") ;;
    esac
done

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "ESP-IDF is not on the path. Source export.sh first:" >&2
    echo "    . ~/esp/esp-idf/export.sh" >&2
    exit 2
fi

# CI's build action exports IDF_TARGET for the one chip it was started for. This
# script sets the target per board, and idf.py refuses a set-target that
# disagrees with the environment, so every board after the first chip's would
# fail for a reason that has nothing to do with the board.
unset IDF_TARGET

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
    [[ $CENSUS -eq 1 ]] && defaults="$defaults;boards/mesh-census.defaults"

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
            # What the census exchange resolved to and linked, read from this
            # board's own sdkconfig and map. On: the option, encrypted NVS and
            # the radio path. Off: no such line and no such symbol.
            cfg="$OUT/$board/sdkconfig"
            map="$OUT/$board/observore.map"
            has_opt=0; has_nvs=0; has_sym=0
            grep -qx 'CONFIG_OBSERVORE_MESH_CENSUS=y' "$cfg" && has_opt=1
            grep -qx 'CONFIG_NVS_ENCRYPTION=y' "$cfg" && has_nvs=1
            grep -q 'observore_espnow_open' "$map" && has_sym=1
            case "$target" in
            esp32s3|esp32c5|esp32c6) can_have=1 ;;
            *)                       can_have=0 ;;
            esac
            if [[ $CENSUS -eq 1 && $can_have -eq 1 ]]; then
                label="census on"
                ok=$((has_opt && has_nvs && has_sym))
                found="option $has_opt, encrypted NVS $has_nvs, radio path linked $has_sym"
                state="off"; should="on"
            else
                [[ $CENSUS -eq 1 ]] && label="census resolved off (classic ESP32)" || label="census off"
                ok=$((! has_opt && ! has_sym))
                found="option $has_opt, radio path linked $has_sym"
                state="on"; should="off"
            fi
            size=$(stat -c%s "$OUT/$board/observore.bin" 2>/dev/null || echo 0)
            if [[ $ok -ne 1 ]]; then
                echo "built, but the census exchange is $state and should be $should ($found)"
                fail+=("$board (census exchange $state)")
            else
                printf 'ok   %s KB  %s\n' "$((size / 1024))" "$label"
                pass+=("$board")
            fi
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

# A gate that built nothing has proven nothing. A board name that matches no
# row is a typo or a board that left the release matrix, and reporting "0 built,
# 0 failed" for it reads as a pass.
if [[ ${#pass[@]} -eq 0 && ${#fail[@]} -eq 0 ]]; then
    if [[ ${#WANT[@]} -gt 0 ]]; then
        echo "no board matched: ${WANT[*]}" >&2
    else
        echo "no board matched" >&2
    fi
    exit 1
fi

echo
echo "${#pass[@]} built, ${#fail[@]} failed"
if [[ ${#fail[@]} -gt 0 ]]; then
    printf '  failed: %s\n' "${fail[*]}"
    exit 1
fi
