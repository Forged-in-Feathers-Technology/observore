#!/usr/bin/env bash
# Fail if a real Wi-Fi credential has reached a tracked file.
#
# The credential options exist in Kconfig, so it is easy to set one with
# menuconfig and later paste it into sdkconfig.defaults "to make it stick" --
# which is exactly how a home network password ends up in a public repo.
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
    # A credential option set to anything other than the empty string.
    if matches=$(grep -nE '^[[:space:]]*CONFIG_OBSERVORE_WIFI_(SSID|PASSWORD)[[:space:]]*=[[:space:]]*"[^"]+"' "$f" 2>/dev/null); then
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
