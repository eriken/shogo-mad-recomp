#!/bin/sh
# Prints the GitHub release notes for a version: its section of CHANGELOG.md,
# then the lines every release carries (what you need, credit and the notice
# Monolith's source licence asks for). Fails if the version has no section.
# Usage: tools/release_notes.sh 0.2.2
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${1:?usage: $0 VERSION}
NOTES=$(awk -v v="$VERSION" '
    /^## / { if (found) exit; found = ($2 == v); next }
    found' "$ROOT/CHANGELOG.md" | sed -e '/./,$!d')
if [ -z "$(printf %s "$NOTES" | tr -d '[:space:]')" ]; then
    echo "CHANGELOG.md has no section for $VERSION (add \"## $VERSION\")" >&2
    exit 1
fi
printf '%s\n' "$NOTES" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
cat <<'END'

---

Needs your own copy of Shogo: Mobile Armor Division from GOG or Steam: open the app and import GOG's installer, or the Steam download (the launcher shows how to get it).

Made by Erik Wessel (shogo-recomp.unlocking364@passmail.net). THIS PORT IS NOT MADE BY OR SUPPORTED BY Monolith Productions, or any of its affiliates and subsidiaries.
END
