#!/usr/bin/env bash
# PostgreSQL 19 from a commit of the git tree, until the PGDG packages carry it: the server of the experimental CI job and the one that job's cache holds
# (.github/actions/pg19). Assertions on (the point of the job), ICU (the collation tests), libxml (the xml type the json tests cast), no debug info, readline or zlib; installed with the TAP modules and the contrib modules the tests use.
#
#   scripts/pg19-build.sh PREFIX COMMIT     fetches that commit only (shallow, from the GitHub mirror: git.postgresql.org dropped a clone with an early EOF)
#
# The recipe is this file: the cache key of the action hashes it, so a change of the flags builds again. ccache is used when its compiler links are installed.
set -euo pipefail

PREFIX=$1
COMMIT=$2
SRC=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/postgresql-XXXXXX")
trap 'rm -rf "$SRC"' EXIT

git -C "$SRC" init -q
git -C "$SRC" fetch -q --depth 1 https://github.com/postgres/postgres.git "$COMMIT"
git -C "$SRC" checkout -q FETCH_HEAD
cd "$SRC"
# The PGDG minors since 17.12 and 18.7 dump pg_attver column options, which a PostgreSQL 19 without attribute versioning rejects when pg_upgrade restores them
# (tests pg_upgrade*): a build without it is never cached. The recipe hash keys the cache, so this check also retires the builds made before it.
[[ -f src/backend/catalog/README.attver.md ]] || { echo "pg19-build: $COMMIT lacks attribute versioning (README.attver.md)" >&2; exit 1; }
# ccache through its compiler links, not CC: the compiler named in the installed pgxs tree is the one that builds the extension against it later
[[ ! -d /usr/lib/ccache ]] || export PATH=/usr/lib/ccache:$PATH
./configure --prefix="$PREFIX" --enable-cassert --enable-tap-tests --with-icu --with-libxml --without-readline --without-zlib CFLAGS="-O1" >/dev/null
make -j"$(nproc)" -s
make -s install
# what the suites use besides the server: the TAP modules and the contrib modules
make -s -C src/test/perl install
for m in amcheck pg_walinspect dblink pageinspect pg_visibility test_decoding; do make -s -C contrib/$m install; done
echo "$COMMIT" > "$PREFIX/.commit"
