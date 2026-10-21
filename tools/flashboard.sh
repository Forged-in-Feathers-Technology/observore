#!/bin/bash
#
# Build one board profile and flash it, refusing to flash if what was built is
# not what was asked for.
#
# `idf.py -DSDKCONFIG_DEFAULTS=...` silently ignores the profile when an
# sdkconfig already exists, and `idf.py set-target` writes one.  So a stale
# sdkconfig -- from the last board, or from a set-target a moment earlier --
# produces a clean, successful build of the wrong firmware, and the first
# symptom is a dark screen on a board you have just reflashed.  That has
# happened three times in this project: twice the 2.8" image onto the 3.5"
# board, once the XIAO image onto a Waveshare AMOLED board.
#
# The fix is to check rather than to remember: the profile sets
# CONFIG_OBSERVORE_BOARD, so the generated sdkconfig either names the board
# that was asked for or nothing is flashed.
#
#   tools/flashboard.sh cyd-3248s035r-st7796 /dev/ttyUSB0
#
set -euo pipefail

BOARD=${1:?usage: flashboard.sh <board> <port>}
PORT=${2:?usage: flashboard.sh <board> <port>}
cd "$(dirname "$0")/.."

# Same board-to-chip mapping the CI matrix uses.  A profile does not name its
# own target, and building for the wrong one fails in ways that look like
# source errors (SPI3_HOST undeclared, when the C5 has only SPI2).
case "$BOARD" in
    cyd-*)                  TARGET=esp32   ;;
    xiao-esp32c5|devkit-*|nm-cyd-c5) TARGET=esp32c5 ;;
    xiao-esp32c6)           TARGET=esp32c6 ;;
    xiao-esp32s3|waveshare-s3-*|crowpanel-*) TARGET=esp32s3 ;;
    *) echo "unknown board: $BOARD" >&2; exit 2 ;;
esac

# Which board is actually on the end of the cable.
#
# This script has always verified the build and never the target, which is
# half the job: it confirms the image matches the board asked for and has no
# idea what is plugged in. Ports renumber whenever something is unplugged,
# and the fourth time firmware went to the wrong board, both of them were
# C5s -- so even a chip check would not have caught it. The MAC is what
# distinguishes two boards of the same kind.
#
# The map is local and untracked, because it describes one bench rather than
# the project: lines of "MAC board" in ~/.observore-boards. With no entry for
# a MAC the flash proceeds and the address is printed, so the file can be
# built up by pasting what it reports.
KNOWN=~/.observore-boards

# A port that is not there is said out loud rather than discovered later.
# Boards get unplugged and renumbered constantly on this bench, and asking
# esptool to talk to a device node that does not exist produces a wall of
# retries and then a failure that reads like a cable fault.
if [[ ! -e "$PORT" ]]; then
    echo "no such port: $PORT" >&2
    echo "ports present: $(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | tr '\n' ' ')" >&2
    exit 2
fi

mac=$(esptool.py --port "$PORT" read_mac 2>/dev/null |
      grep -oE "^MAC: [0-9a-f:]+" | head -1 | cut -d' ' -f2) || true
if [[ -n "$mac" && -f "$KNOWN" ]]; then
    # `|| true` because grep exits 1 when the address is not in the file, and
    # under `set -e` with `pipefail` that status becomes the assignment's and
    # kills the script -- silently, before anything has been printed.
    #
    # That is the same failure the note below describes, one line later and
    # left behind when it was fixed there. It meant the case this file's own
    # header promises -- "with no entry for a MAC the flash proceeds and the
    # address is printed, so the file can be built up by pasting what it
    # reports" -- had never once worked: every genuinely new board, which is
    # exactly when you need that line printed, exited 1 with no output at all.
    # Found by plugging in a board the file had never seen.
    want=$(grep -i "^$mac " "$KNOWN" | awk '{print $2}' | head -1 || true)
    if [[ -n "$want" && "$want" != "$BOARD" ]]; then
        echo "REFUSING to flash: $PORT is $mac, which is $want, not $BOARD" >&2
        exit 1
    fi
    if [[ -z "$want" ]]; then
        echo "  $PORT is $mac, which $KNOWN does not list" >&2
        echo "  once this board is seen working, add:  $mac $BOARD" >&2
    fi
fi
# An `if` rather than `[[ ... ]] && echo`, which under `set -e` is a silent
# exit 1 when the address could not be read -- the script printed nothing at
# all for a port that had been unplugged, which is how this was found.
if [[ -n "$mac" ]]; then
    echo "  $PORT is $mac"
else
    echo "  $PORT did not answer with an address; flashing without the guard" >&2
fi

PROFILE="boards/$BOARD.defaults"
DEFAULTS="sdkconfig.defaults"
[ -f "$PROFILE" ] && DEFAULTS="sdkconfig.defaults;$PROFILE"

# An optional extra fragment, last so it wins, for trying an option on real
# hardware before it is anybody's default -- OBSERVORE_CONSOLE_HTTPS was the
# first. It is appended rather than substituted, so the board profile is still
# read and the CONFIG_OBSERVORE_BOARD check below still has something to
# verify: a fragment cannot be used to flash the wrong board quietly.
#
#   OBSERVORE_EXTRA_DEFAULTS=/tmp/https.defaults \
#     tools/flashboard.sh waveshare-s3-amoled-143 /dev/ttyACM2
if [[ -n "${OBSERVORE_EXTRA_DEFAULTS:-}" ]]; then
    if [[ ! -f "$OBSERVORE_EXTRA_DEFAULTS" ]]; then
        echo "no such defaults fragment: $OBSERVORE_EXTRA_DEFAULTS" >&2
        exit 2
    fi
    DEFAULTS="$DEFAULTS;$OBSERVORE_EXTRA_DEFAULTS"
    echo "  plus $OBSERVORE_EXTRA_DEFAULTS"
fi

# set-target first, because it writes an sdkconfig; then remove it, so the
# profile below is actually read.  Order matters and is the whole point.
# Quiet while it works, loud when it does not.
#
# These used to send both streams to /dev/null unconditionally, so a build
# that failed exited this script through `set -e` with no output at all --
# three separate times during the 1.85 bring-up it looked like the guard
# refusing rather than the compiler objecting. A build log nobody can see is
# a build log that costs somebody twenty minutes.
build_log=$(mktemp)
trap 'rm -f "$build_log"' EXIT
if ! { idf.py set-target "$TARGET" &&
       rm -f sdkconfig &&
       idf.py -DSDKCONFIG_DEFAULTS="$DEFAULTS" build; } >"$build_log" 2>&1; then
    echo "build failed for $BOARD:" >&2
    grep -E "error:|Error|FAILED|CMake Error" "$build_log" | head -20 >&2 ||
        tail -20 "$build_log" >&2
    exit 1
fi

if ! grep -qxF "CONFIG_OBSERVORE_BOARD=\"$BOARD\"" sdkconfig; then
    echo "REFUSING to flash: sdkconfig says $(grep '^CONFIG_OBSERVORE_BOARD=' sdkconfig), not $BOARD" >&2
    exit 1
fi

# Stopping after the checks, for the case where the flash itself has to be
# driven by something else -- capturing the very first boot needs the port held
# across the reset, which `idf.py flash` cannot do because it resets and exits.
# The guards above have all run by this point; only the write is skipped.
if [[ -n "${OBSERVORE_BUILD_ONLY:-}" ]]; then
    echo "  verified $BOARD on $TARGET, NOT flashing (OBSERVORE_BUILD_ONLY)"
    echo "  flash args: build/flash_args"
    exit 0
fi

echo "  verified $BOARD on $TARGET, flashing $PORT"
idf.py -p "$PORT" flash 2>&1 | grep -E "error|Hard resetting" | tail -1
