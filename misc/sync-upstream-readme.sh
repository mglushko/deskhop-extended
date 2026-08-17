#!/usr/bin/env bash
#
# Rebuild README.md as this fork's section (everything down to the marker) followed by
# hrvach/deskhop's README verbatim.
#
# The two halves are kept strictly separated so upstream's half stays byte-identical to its
# source, which is what lets upstream's edits merge without a conflict. The one case git
# cannot work out on its own is an edit to the very first lines of upstream's README: our
# section is prepended there, so upstream's line 1 has no context above it to anchor on.
# Don't hand-merge that. Take our side and re-run this script:
#
#     git checkout --ours README.md && misc/sync-upstream-readme.sh && git add README.md
#
# Usage: misc/sync-upstream-readme.sh [upstream-ref]      (default: upstream/main)

set -euo pipefail

ref="${1:-upstream/main}"
marker='<!-- upstream-readme-below'

cd "$(git rev-parse --show-toplevel)"

if ! grep -qF "$marker" README.md; then
    echo "error: marker '$marker' not found in README.md - has the layout changed?" >&2
    exit 1
fi

if ! git rev-parse --quiet --verify "$ref:README.md" >/dev/null; then
    echo "error: no README.md at '$ref' - try: git fetch upstream" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

sed -n "1,/^$(sed 's/[][\.*^$/]/\\&/g' <<<"$marker")/p" README.md > "$tmp/fork.md"
git show "$ref:README.md" > "$tmp/upstream.md"
printf '\n' | cat "$tmp/fork.md" - "$tmp/upstream.md" > README.md

echo "README.md rebuilt: fork section ($(wc -l < "$tmp/fork.md") lines) + ${ref} README ($(wc -l < "$tmp/upstream.md") lines)"
