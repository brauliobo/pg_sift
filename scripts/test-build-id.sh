#!/usr/bin/env bash
# The module version (scripts/build-id.sh) names the release tag even when a newer tag of another kind (a base-*, rc-* or lane marker) is the nearest one: it starts with a digit, ends in +SHA. Seconds, no server.
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
git init -q "$TMP/t" && cd "$TMP/t" || exit 1
git config user.email t@example.com; git config user.name t
commit() { git commit -q --allow-empty -m "$1"; }
commit one; git tag v1.2.3; commit two; git tag base-1.2.4; commit three
id=$("$REPO/scripts/build-id.sh" "$TMP/t")
[[ $id =~ ^1\.2\.3-2-g[0-9a-f]{7,}\+[0-9a-f]{7,}$ ]] && echo "ok: $id" || { echo "FAIL: build id '$id' does not name release v1.2.3"; exit 1; }
