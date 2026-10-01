#!/usr/bin/env bash
# What did the last push actually do?
#
# This exists because eight of sixteen builds were broken for ten commits and
# nothing here noticed. Every local build during those ten commits was for a
# board sitting on the desk, and every board on the desk has a screen -- so
# the builds that passed locally were exactly the ones that could not fail.
# The ones that were broken were the headless targets nobody had plugged in.
#
# The lesson is not "check CI more often", which is advice rather than a
# mechanism. It is that a green tick on a machine that only builds what is in
# front of it means nothing, and the question "did the remote agree" has an
# answer a command can fetch.
#
#   tools/check_ci.sh          # the latest run for this branch
#   tools/check_ci.sh --wait   # wait for it to finish first
#
# Exits non-zero when the run failed, so it can gate a merge or a tag.
set -euo pipefail

cd "$(dirname "$0")/.."

branch=$(git rev-parse --abbrev-ref HEAD)
wait=0
[[ "${1:-}" == "--wait" ]] && wait=1

if ! command -v gh >/dev/null; then
    echo "gh is not installed; cannot ask about $branch" >&2
    exit 2
fi

fetch() {
    gh run list --branch "$branch" --limit 1 \
       --json status,conclusion,displayTitle,url \
       --jq '.[0] // empty | [.status, (.conclusion // "-"),
                              (.displayTitle // "(untitled)"), .url]
             | join("\u001f")' 2>/dev/null
}

row=$(fetch)
if [[ -z "$row" ]]; then
    echo "no run found for $branch -- has it been pushed?" >&2
    exit 2
fi

if (( wait )); then
    for _ in $(seq 1 60); do
        status=${row%%$'\x1f'*}
        [[ "$status" == "completed" ]] && break
        sleep 20
        row=$(fetch)
    done
fi

# A unit separator rather than a tab: tab is whitespace to read, so an empty
# field collapses into the next one and the columns silently shift.
IFS=$'\x1f' read -r status conclusion title url <<<"$row"

case "$status:$conclusion" in
    completed:success)
        echo "green: $title"
        exit 0
        ;;
    completed:*)
        echo "FAILED ($conclusion): $title"
        echo "  $url"
        # Name the jobs rather than the run, because "CI is red" is not
        # actionable and "the four headless builds are red" is.
        gh run view --branch "$branch" --json jobs \
           --jq '.jobs[] | select(.conclusion != "success" and .conclusion != null and .conclusion != "skipped") | "  - \(.name)"' \
           2>/dev/null || true
        exit 1
        ;;
    *)
        echo "still running: $title"
        echo "  $url"
        exit 3
        ;;
esac
