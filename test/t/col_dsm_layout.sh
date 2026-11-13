#!/usr/bin/env bash
# The shared tables of the server (col/dsmreg.c) are told apart by more than the size of their struct: two libraries whose struct has the same size and a
# different layout (a field moved into padding, another meaning for a field) must not share a table, and the tables of the libraries a server has seen must
# not pile up: a package upgrade without a restart, or a setting that sets the size, makes a table nobody maps any more.
# sift._test_dsm_layout(n, name, size) is the table `name` as the library of layout n asks for it; its result is stamp * 1000000 + calls.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 8
node_init ds 79
node_start ds || bail "start failed"
sql ds "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

layout() { sql ds "SELECT sift._test_dsm_layout($1, '$2', 4096)"; }
# the segments of the server of one size: a table of that size is one file of that size
segments() { find /dev/shm -maxdepth 1 -name 'PostgreSQL.*' -size +"$(($1 - 1))"c -size -"$(($1 + $2))"c 2>/dev/null | wc -l; }

out=$(layout 1 lay_probe)
check "the first library makes the table" test "$out" == 1000001
out=$(layout 2 lay_probe)
check "a library of another layout and the same size gets a table of its own" test "$out" == 2000001
out=$(layout 2 lay_probe)
check "and the backends of one layout share it" test "$out" == 2000002

# a library of the first layout still runs (its backend holds the table) while the second asks: neither loses its table to the other
bg=$(mktemp)
sql ds "SELECT sift._test_dsm_layout(1, 'lay_live', 4096)" "SELECT pg_sleep(3)" "SELECT sift._test_dsm_layout(1, 'lay_live', 4096)" >"$bg" &
pid=$!
sleep 1
out=$(layout 2 lay_live)
check "a layout another backend still maps is kept when a second layout asks" test "$out" == 2000001
out=$(layout 1 lay_live)
check "and its backends find that table, not a new one" test "$out" == 1000002
wait $pid
check "the backend that held it kept it" test "$(tail -n 1 "$bg")" == 1000003
rm -f "$bg"

BASE=3000000
for i in $(seq 1 12); do sql ds "SELECT sift._test_dsm_layout($i, 'lay_leak', $((BASE + 64 * i)))" >/dev/null; done
out=$(segments $BASE 1000)
check "tables of 12 libraries, one after the other, do not pile up (segments: $out)" test "$out" -le 2
BASE=3100000
for i in $(seq 1 12); do sql ds "SELECT sift._test_dsm_layout(1, 'lay_size', $((BASE + 64 * i)))" >/dev/null; done
out=$(segments $BASE 1000)
check "nor do the tables of 12 sizes of one setting (segments: $out)" test "$out" -le 2
finish
