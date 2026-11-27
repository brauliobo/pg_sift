#!/usr/bin/env bash
# The cuts of the end of a sift_col file that a reader's lock keeps from running (the heap is not cut, whose wait would be PostgreSQL's): the metapage of Format 2.0 counts them, and sift.space, sift.status and sift.health name why the
# blocks stay (lock, snapshot) whatever the count, so that sift.health never reads a name that is not one (a backend read past the names and segfaulted after a few cuts).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init p 4 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
sift.async_maintenance = off"
node_start p || bail "start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE TABLE sh (id int, v int)" "INSERT INTO sh SELECT g, g FROM generate_series(1, 300000) g" \
	"CREATE INDEX sh_ix ON sh USING sift_col (id, v)" "DELETE FROM sh WHERE id > 1000" >/dev/null || bail "setup"
D=$(dir p)
sess_open p rd
sess_say rd "BEGIN; SELECT count(*) FROM sh WHERE id < 0; \\echo held"
wait_for "grep -q '^held$' '$D/rd.out'" 30 || bail "the reader did not take its lock"
for _ in 1 2 3 4 5; do sql p "VACUUM (TRUNCATE false) sh" >/dev/null || bail "vacuum"; done
out=$(sql p "SELECT shrink_blocked_by FROM sift.space('sh_ix')" "SELECT shrink_blocked_by FROM sift.status('sh_ix')")
check "five cuts kept by a reader's lock: sift.space and sift.status say why" grep -qxE "(lock|snapshot)"$'\n'"(lock|snapshot)" <<<"$out"
out=$(sql p "SELECT check_name FROM sift.health('sh_ix') WHERE check_name = 'tail_shrink_blocked'" 2>&1)
check "and sift.health reads it without an error" test $? -eq 0
sess_say rd "COMMIT;"
sess_close rd
out=$(sql p "VACUUM sh" "SELECT coalesce(shrink_blocked_by, 'none') FROM sift.status('sh_ix')")
check "a cut that runs clears it" test "$out" == "none"
finish
