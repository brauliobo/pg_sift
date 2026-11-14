#!/usr/bin/env bash
# A session keeps the library it loaded first: a second file of pg_sift (the library of another path, which is what a session meets when the files are replaced in a
# way the server resolves again) must fail with a message that says what happened and to reconnect, not with "attempt to redefine parameter".
. "$(dirname "$0")/lib.bash"

plan 3
node_init a 37 "autovacuum = off"
node_start a || bail "start failed"
D=$(dir a)
S=("-h" "$D/sock" "-p" "$(port a)" "-U" "postgres")
mkdir -p "$D/other"
cp "$PKGLIBDIR/pg_sift$DLSUFFIX" "$D/other/pg_sift$DLSUFFIX"
out=$(psql -X -At "${S[@]}" -d postgres -c "CREATE EXTENSION pg_sift" -c "SELECT sift.version()" -c "LOAD '$D/other/pg_sift$DLSUFFIX'" 2>&1)
check "a second file of the library is refused with a message that names the builds" test "$(grep -c 'this session already holds pg_sift build .* and cannot load build' <<<"$out")" == 1
check "the message says to reconnect, and is no redefined setting" test "$(grep -c 'HINT:  Reconnect' <<<"$out")" == 1 -a "$(grep -c 'attempt to redefine' <<<"$out")" == 0
out=$(psql -X -At "${S[@]}" -d postgres -c "SELECT sift.version()" 2>&1)
check "a new session works" test -n "$out" -a "$(grep -c ERROR <<<"$out")" == 0
finish
