#!/usr/bin/env bash
# A connected pair made by this tree (the parent index keeps a key journal, column format minor 44) and a release that does not know the minor: the release refuses the maintenance of the index (VACUUM, merge, promotion) at the
# directory, where a segment format it does not know is invalid; this tree reads the pair again. (Appends of tombstones by that release are not refused: see docs/COL-CONNECT.md.)
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v4.[12].*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v4.1.* or v4.2.*'; exit 0; }
newroot=$ROOT
plan 5

build_old "$ref"; oldroot=$OLDROOT
node_init dg 43 'autovacuum = off'
use_root dg "$newroot"
node_start dg || bail "cannot start this tree"
sql dg "CREATE EXTENSION pg_sift" "CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text)" \
	"INSERT INTO dim_a SELECT i, i % 10, 'l' || i FROM generate_series(1, 500) i" "CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, value_num int NOT NULL)" \
	"INSERT INTO events SELECT i, 1 + i % 500, i FROM generate_series(1, 2000) i" "CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label)" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, value_num) WITH (connect = 'dim_a_id -> dim_a(id)')" \
	"UPDATE dim_a SET category_id = category_id + 1 WHERE id % 50 = 0" >/dev/null 2>&1 || bail "the pair cannot be made"
A="SELECT count(*) || ':' || sum(category_id) FROM dim_a"
check "this tree: the parent index has its journal and a stamp that vouches" test "$(sql dg "SELECT stamp FROM sift.col_connections")" == ok
node_stop dg

use_root dg "$oldroot"
node_start dg || bail "cannot start $ref"
sql dg "SELECT sift.col_merge('dim_a_col', true)" >"$TAPDIR/old.out" 2>&1
check "$ref refuses a maintenance call on the parent index: the directory names a segment format it does not know" test "$(grep -c 'unknown segment format' "$TAPDIR/old.out")" == 1
sql dg "VACUUM dim_a" >"$TAPDIR/old_vacuum.out" 2>&1
check "$ref refuses the VACUUM of the table of the parent index the same way" test "$(grep -c 'unknown segment format' "$TAPDIR/old_vacuum.out")" == 1
check "$ref reads the table without the index" test "$(sql dg "SET enable_indexscan = off" "SET enable_bitmapscan = off" "$A" 2>&1 | tail -n 1 | grep -c ':')" == 1
node_stop dg

use_root dg "$newroot"
node_start dg || bail "cannot start this tree again"
check "this tree reads the pair again: the same connection and a stamp that vouches" test "$(sql dg "SELECT child || ' ' || parent FROM sift.col_connections")" == "events_col dim_a_col"
finish
