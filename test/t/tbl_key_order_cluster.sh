#!/usr/bin/env bash
# The key_order option of a sift_table (docs/COL-TAM.md section 8.2) across what moves a table: a table of 4.3.0 read and updated unchanged, crash
# recovery of the sorted rewrite (finished and cut), a hot standby that replays it, parallel scans of a sorted table, pg_dump | psql and pg_upgrade.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=v4.3.0
git -C "$REPO" rev-parse -q --verify "refs/tags/$ref" >/dev/null || { echo "1..0 # SKIP no release tag $ref"; exit 0; }
newroot=$ROOT
N=$(knob SIFT_KO_ROWS 300000 90000)

LOAD="INSERT INTO t SELECT i, (hashint8(i) & 2147483647) % 500, timestamptz '2024-01-01' + ((hashint8(i * 3) & 2147483647) % 100000) * interval '1 minute', 'v' || i % 97
	FROM generate_series(1, $N) i"
# what the rows are, however they are stored: one string
ROWS="SELECT md5(string_agg(t::text, '|' ORDER BY id)) || ' ' || count(*) FROM t"
# the rows in the order of the table as a string of keys: sorted when it is the same as the key sorted
PHYS="SELECT (SELECT md5(string_agg(k::text, ',' ORDER BY ctid)) FROM t) = (SELECT md5(string_agg(k::text, ',' ORDER BY k)) FROM t)"
UPK="SELECT round(units_per_key::numeric, 1) FROM sift.table_layouts('t')"
rows() { sql "$1" "$ROWS" | tail -n 1; }
sorted() { sql "$1" "$PHYS" | tail -n 1; }
upk() { sql "$1" "$UPK" | tail -n 1; }

build_old "$ref"; oldroot=$OLDROOT
node_init a 61 'autovacuum = off
wal_level = replica
max_wal_senders = 4'
use_root a "$oldroot"
node_start a || bail "cannot start $ref"
sql a "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, k int, ts timestamptz, s text) USING sift_table" "$LOAD" "VACUUM t" >/dev/null 2>&1 || bail "$ref cannot be set up"
base=$(rows a)
node_stop a

use_root a "$newroot"
node_start a || bail "cannot start this tree on $ref data"
check "a table of $ref reads unchanged with this library" test "$(rows a)" == "$base"
sql a "ALTER EXTENSION pg_sift UPDATE" >/dev/null 2>"$TAPDIR/upd.err"
check "ALTER EXTENSION UPDATE from $ref succeeds" test -z "$(cat "$TAPDIR/upd.err")"
check "it reads unchanged after the update, and the table has no key_order" test "$(rows a)|$(sql a "SELECT key_order = 'load order' FROM sift.table_layouts('t')" | tail -n 1)" == "$base|t"
sql a "INSERT INTO t VALUES (-1, 1, now(), 'new')" "DELETE FROM t WHERE id = -1" "VACUUM FULL t" >/dev/null 2>&1
check "VACUUM FULL of a table with no option keeps its rows and its order of load" test "$(rows a)" == "$base"
check "setting the option on the table of $ref succeeds and changes no row" test "$(sql a "SELECT sift.table_set_option('t', 'key_order', 'k')" >/dev/null 2>&1; rows a)" == "$base"
check "it is scattered before the rewrite" test "$(sorted a)" == f
sql a "VACUUM FULL t" >/dev/null 2>&1
check "VACUUM FULL sorts it and keeps every row" test "$(sorted a)|$(rows a)|$(upk a)" == "t|$base|1.0"
base=$(rows a)

# parallel scans of a sorted table answer as the serial ones
PAR="SET sift.enable_agg = off"; PAR1="SET parallel_setup_cost = 0" ; PAR2="SET parallel_tuple_cost = 0"; PAR3="SET min_parallel_table_scan_size = 0"
AGG="SELECT k, count(*), sum(id) FROM t GROUP BY k ORDER BY k"
serial=$(sql a "SET max_parallel_workers_per_gather = 0" "$PAR" "SELECT md5(string_agg(x::text, ',')) FROM ($AGG) x" | tail -n 1)
par=$(sql a "SET max_parallel_workers_per_gather = 4" "$PAR" "$PAR1" "$PAR2" "$PAR3" "SELECT md5(string_agg(x::text, ',')) FROM ($AGG) x" | tail -n 1)
check "a parallel aggregate over the sorted table answers as the serial one" test "$par" == "$serial"
check "the plan of it is parallel" test "$(sql a "SET max_parallel_workers_per_gather = 4" "$PAR" "$PAR1" "$PAR2" "$PAR3" "EXPLAIN (COSTS OFF) $AGG" | grep -c 'Parallel')" -gt 0
check "a parallel count of one key reads the rows of the key" test "$(sql a "SET max_parallel_workers_per_gather = 4" "$PAR" "$PAR1" "$PAR2" "$PAR3" "SELECT count(*) FROM t WHERE k = 7" | tail -n 1)" == "$(sql a "SET max_parallel_workers_per_gather = 0" "SELECT count(*) FROM t WHERE k = 7" | tail -n 1)"

# crash recovery: a finished rewrite survives kill -9, a rewrite that is cut leaves the table as it was
sql a "INSERT INTO t SELECT 1000000 + i, (hashint8(i * 11) & 2147483647) % 500, now(), 'y' FROM generate_series(1, $((N / 5))) i" "VACUUM t" >/dev/null 2>&1
base=$(rows a)
check "rows loaded after the rewrite drift the table: it says so and reads more units for a key" test "$(sorted a)|$(( $(upk a | tr -d .) > 10 ))" == "f|1"
sql a "VACUUM FULL t" "CHECKPOINT" >/dev/null 2>&1
check "the finished rewrite sorted the table" test "$(sorted a)" == t
sql a "INSERT INTO t VALUES (-2, 1, now(), 'wal')" >/dev/null 2>&1
base=$(rows a)
crash a
node_start a || bail "node a did not restart after the crash"
check "after kill -9 of a finished rewrite the table has its rows, and the one written after it" test "$(rows a)" == "$base"
check "... and the row written after it is there" test "$(sql a "SELECT count(*) FROM t WHERE id = -2" | tail -n 1)" == 1
sql a "DELETE FROM t WHERE id = -2" "INSERT INTO t SELECT 2000000 + i, (hashint8(i * 13) & 2147483647) % 500, now(), 'z' FROM generate_series(1, $((N / 5))) i" "VACUUM t" >/dev/null 2>&1
base=$(rows a)
# the rewrite is cut where it publishes its segments (the first publication of the statement): a rewrite of this size is over before a poll could find it
pm=$(head -1 "$(dir a)/data/postmaster.pid")
SIFT_TMP=$(dir a) leak_expect 1 'terminated by signal 9'
sql a "SET sift.test_crash_dir = 1" "VACUUM FULL t" >/dev/null 2>&1
wait_for "! kill -0 $pm 2>/dev/null" 20 || bail "the rewrite was not cut"
node_start a || bail "node a did not restart after the cut rewrite"
check "after kill -9 in the rewrite the table has its rows, none twice" test "$(rows a)" == "$base"
check "... and it takes the rewrite again" test "$(sql a "VACUUM FULL t" >/dev/null 2>&1; sorted a)|$(rows a)" == "t|$base"

# a hot standby replays the sorted rewrite
node_clone a b 62 "hot_standby_feedback = on"
node_start b || bail "standby did not start"
sql a "INSERT INTO t SELECT 3000000 + i, (hashint8(i * 17) & 2147483647) % 500, now(), 'w' FROM generate_series(1, $((N / 5))) i" "VACUUM t" "VACUUM FULL t" >/dev/null 2>&1
catchup a b || bail "the standby did not catch up"
check "the standby has the rows of the primary" test "$(rows b)" == "$(rows a)"
check "the standby reads the table sorted, as the primary" test "$(sorted b)|$(upk b)" == "$(sorted a)|$(upk a)"
check "the standby has the option" test "$(sql b "SELECT sift.table_get_option('t', 'key_order')" | tail -n 1)" == k
node_stop b

# pg_dump | psql: the option comes with the table and the next rewrite sorts it
sql a "CREATE DATABASE r1" >/dev/null
pg_dump -h "$(dir a)/sock" -p "$(port a)" -U postgres -d postgres | psql -X -q -h "$(dir a)/sock" -p "$(port a)" -U postgres -d r1 >"$TAPDIR/r1.log" 2>&1
check "pg_dump | psql succeeds" test ${PIPESTATUS[1]} -eq 0
check "the restored table has its option and its rows" test "$(psql -X -At -h "$(dir a)/sock" -p "$(port a)" -U postgres -d r1 -c "SELECT sift.table_get_option('t', 'key_order')" -c "SELECT count(*) FROM t" | tr '\n' ' ')" == "k $(rows a | cut -d' ' -f2) "
before=$(rows a)

# pg_upgrade
if [[ ! -f $(pg_config --pkglibdir)/pg_sift$DLSUFFIX ]]; then
	skip 3 "pg_upgrade needs sift installed in $(pg_config --pkglibdir)"
	plan $NTEST
	finish
fi
node_stop a
node_init up 8
BIN=$(pg_config --bindir)
(cd "$(dir up)" && LD_PRELOAD=${SIFT_PRELOAD:-} pg_upgrade -b "$BIN" -B "$BIN" -d "$(dir a)/data" -D "$(dir up)/data" \
	-U postgres -p "$(port a)" -P "$(port up)" -s "$(dir up)/sock" >"$(dir up)/pg_upgrade.log" 2>&1)
upgraded=$?
check "pg_upgrade of the database with a key_order succeeds" test $upgraded -eq 0
[[ $upgraded -eq 0 ]] || diag "$(tail -n 15 "$(dir up)/pg_upgrade.log")"
node_start up || bail "upgraded cluster did not start"
check "the rows and the option are those of the source after pg_upgrade" test "$(rows up)|$(sql up "SELECT sift.table_get_option('t', 'key_order')" | tail -n 1)" == "$before|k"
check "the upgraded table rewrites sorted" test "$(sql up "INSERT INTO t SELECT 4000000 + i, i % 500, now(), 'u' FROM generate_series(1, 5000) i" "VACUUM FULL t" >/dev/null 2>&1; sorted up)" == t
plan $NTEST
finish
