#!/usr/bin/env bash
# Fail if a credential has reached a tracked file.
#
# Covers the uplink SSID and password, and the console SoftAP password.  The
# last one matters even though it is not "yours": each device generates its own
# on first boot, so a value committed here would put every device built from
# this firmware behind one shared, published password.
#
# The options exist in Kconfig, so it is easy to set one with menuconfig and
# later paste it into sdkconfig.defaults "to make it stick" -- which is exactly
# how a home network password ends up in a public repo.
#
#   tools/check_no_secrets.sh          # check tracked files
#   tools/check_no_secrets.sh --staged # check what is about to be committed
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ "${1:-}" == "--staged" ]]; then
    files=$(git diff --cached --name-only --diff-filter=ACM)
else
    files=$(git ls-files)
fi

status=0
for f in $files; do
    [[ -f "$f" ]] || continue

    # A real generated console password, pasted into documentation.
    #
    # This is not hypothetical: the README and the flasher page both once
    # carried a live device's SoftAP name and WPA2 key, because the "example"
    # was copied straight out of a serial log.  Documenting the format means
    # showing a sample, and the nearest sample to hand is always a real one.
    #
    # Generated passwords are 12 characters of "abcdefghjkmnpqrstuvwxyz23456789"
    # (no i/l/o/0/1, to stay readable off a serial log).  A placeholder of
    # repeated x's is the documented stand-in and is allowed through.
    if matches=$(grep -nE 'password: [abcdefghjkmnpqrstuvwxyz23456789]{12}' "$f" 2>/dev/null \
                 | grep -vE 'password: x{12}'); then
        echo "error: what looks like a real console password is in $f" >&2
        echo "$matches" | sed 's/^/  /' >&2
        echo "  use 'password: xxxxxxxxxxxx' in documentation" >&2
        status=1
    fi

    # A real SoftAP name gives away the device's MAC suffix, and pairs with the
    # password above to identify exactly which device was exposed.
    if matches=$(grep -nE '"console-[0-9A-F]{6}"' "$f" 2>/dev/null \
                 | grep -vE '"console-X{6}"'); then
        echo "error: a real console SoftAP name is in $f" >&2
        echo "$matches" | sed 's/^/  /' >&2
        echo "  use \"console-XXXXXX\" in documentation" >&2
        status=1
    fi
    # A credential option set to anything other than the empty string.
    if matches=$(grep -nE '^[[:space:]]*CONFIG_OBSERVORE_(WIFI_SSID|WIFI_PASSWORD|AP_PASSWORD)[[:space:]]*=[[:space:]]*"[^"]+"' "$f" 2>/dev/null); then
        # The example file is allowed to carry obvious placeholders.
        if [[ "$f" == "credentials.conf.example" ]]; then
            continue
        fi
        echo "error: credential in tracked file $f" >&2
        echo "$matches" | sed 's/^/  /' >&2
        status=1
    fi
done

if [[ $status -ne 0 ]]; then
    cat >&2 <<'MSG'

Credentials must not be committed.  Use one of:
  idf.py menuconfig                       (writes sdkconfig, which is gitignored)
  cp credentials.conf.example credentials.conf   (gitignored)
  set the network at runtime from the console    (stored in NVS)
MSG
else
    echo "no credentials in tracked files"
fi
exit $status
