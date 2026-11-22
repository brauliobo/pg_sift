#!/usr/bin/env bash
# Builds the v5.1.0 release library and SQL once, into a staged root that test/t/031_v51_full_compat.pl installs
# under a throwaway server, so the Format 1 relations the test writes come from the real release.
#
#   scripts/build-v510-fixture.sh [--force] [OUT]
#
# OUT (default /tmp/sift-v510-lib, or $SIFT_V510_LIBDIR) gets the layout of a DESTDIR install:
#   OUT/<pkglibdir>/pg_sift$DLSUFFIX   OUT/<sharedir>/extension/pg_sift--5.1.sql, pg_sift.control, ...
# The release comes from `git archive $SIFT_V510_REF` (default v5.1.0) of the repository $SIFT_V510_SRC (default: the
# repository of this script); the checkout is never touched. PG_CONFIG selects the server (default pg_config).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
FORCE=
[[ ${1:-} != --force ]] || { FORCE=1; shift; }
OUT=${1:-${SIFT_V510_LIBDIR:-/tmp/sift-v510-lib}}
REF=${SIFT_V510_REF:-v5.1.0}
PG_CONFIG=${PG_CONFIG:-pg_config}

sha=
for REPO in ${SIFT_V510_SRC:+"$SIFT_V510_SRC"} "$HERE/.." "$HERE/../../wt-510"; do
	sha=$(git -C "$REPO" rev-parse --verify --quiet "$REF^{commit}" 2>/dev/null) && break || sha=
done
[[ -n $sha ]] || { echo "build-v510-fixture: no $REF found (set SIFT_V510_SRC to a repository that has the tag)" >&2; exit 1; }
major=$("$PG_CONFIG" --version | sed 's/[^0-9]*\([0-9]*\).*/\1/')
stamp="$sha pg$major"

if [[ -z $FORCE && -f $OUT/.fixture && $(cat "$OUT/.fixture") == "$stamp" ]]; then
	echo "build-v510-fixture: $OUT is current ($stamp)"
	exit 0
fi

WORK=$(mktemp -d /tmp/sift-compat-build.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

git -C "$REPO" archive "$REF" | tar -x -C "$WORK"
rm -rf "$OUT"
mkdir -p "$OUT"
nice make -C "$WORK" -j4 USE_PGXS=1 PG_CONFIG="$PG_CONFIG" SIFT_TESTING= >"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log" >&2; echo "build-v510-fixture: the build of $REF failed for PostgreSQL $major" >&2; exit 1; }
nice make -C "$WORK" USE_PGXS=1 PG_CONFIG="$PG_CONFIG" SIFT_TESTING= DESTDIR="$OUT" install >>"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log" >&2; exit 1; }

lib=$("$PG_CONFIG" --pkglibdir)
share=$("$PG_CONFIG" --sharedir)
compgen -G "$OUT$lib/pg_sift.*" >/dev/null || { echo "build-v510-fixture: no pg_sift library under $OUT$lib" >&2; exit 1; }
[[ -f $OUT$share/extension/pg_sift--5.1.sql ]] || { echo "build-v510-fixture: no pg_sift--5.1.sql under $OUT$share/extension" >&2; exit 1; }
echo "$stamp" >"$OUT/.fixture"
echo "build-v510-fixture: $REF ($sha) for PostgreSQL $major staged in $OUT"
