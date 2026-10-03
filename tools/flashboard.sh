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
    xiao-esp32s3|waveshare-s3-*) TARGET=esp32s3 ;;
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
    want=$(grep -i "^$mac " "$KNOWN" | awk '{print $2}' | head -1)
    if [[ -n "$want" && "$want" != "$BOARD" ]]; then
        echo "REFUSING to flash: $PORT is $mac, which is $want, not $BOARD" >&2
        exit 1
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

# set-target first, because it writes an sdkconfig; then remove it, so the
# profile below is actually read.  Order matters and is the whole point.
idf.py set-target "$TARGET" >/dev/null 2>&1
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="$DEFAULTS" build >/dev/null 2>&1

if ! grep -qxF "CONFIG_OBSERVORE_BOARD=\"$BOARD\"" sdkconfig; then
    echo "REFUSING to flash: sdkconfig says $(grep '^CONFIG_OBSERVORE_BOARD=' sdkconfig), not $BOARD" >&2
    exit 1
fi

echo "  verified $BOARD on $TARGET, flashing $PORT"
idf.py -p "$PORT" flash 2>&1 | grep -E "error|Hard resetting" | tail -1
