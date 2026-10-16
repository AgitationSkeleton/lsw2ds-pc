#!/usr/bin/env bash
# Download the ndsrecomp framework this port builds against, into
# third_party/ndsrecomp (ignored by git), where port/build.sh looks for it.
#
#   bash tools/fetch_ndsrecomp.sh
#
# PINNED TO ONE COMMIT. port/patches/ are diffs against exactly this tree, and
# they were verified to apply cleanly to it in order and to reproduce the tree
# the port is developed on. A newer ndsrecomp is a different program as far as
# the patches are concerned: moving the pin means re-checking every patch.
#
# The framework is MIT licensed, but its runner vendors GPL-3.0-or-later melonDS
# code; see THIRD_PARTY.md.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMMIT="e19dbf442b9215d23a387f0c2430e8c19b8756c0"
URL="https://github.com/RetroPortingToolKit/ndsrecomp/archive/$COMMIT.tar.gz"
DEST="$REPO/third_party/ndsrecomp"

if [ -f "$DEST/recompiler/CMakeLists.txt" ]; then
    echo "ndsrecomp is already at $DEST (delete it to fetch again)"
    exit 0
fi

mkdir -p "$REPO/third_party"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "downloading ndsrecomp $COMMIT"
curl -fsSL "$URL" -o "$tmp/ndsrecomp.tar.gz"
tar -xzf "$tmp/ndsrecomp.tar.gz" -C "$tmp"
mv "$tmp/ndsrecomp-$COMMIT" "$DEST"
echo "ndsrecomp -> $DEST"
echo "port/build.sh applies port/patches/ to it on the first build."
