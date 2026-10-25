#!/usr/bin/env bash
# sift.col_unit_floor: the overlays that churn leaves are units cut by TID range, in a plain index. Crash: a PANIC in the tier merge of overlays that stay sparse and
# in the merge of all of them, with overlays cut by TID range and by superblock among the inputs, leaves the index as it was (verifies, answers the heap's) and the merges,
# asked again, complete. Replica: a hot standby replays the promotions and merges, answers as the primary does, a ctid range included (a unit cut by TID range is no
# block range). Upgrade: the index the last release wrote is read, promoted into and merged by this one, which cuts the overlays by TID range from then on.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

node_init fl 26 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off
max_parallel_maintenance_workers = 0
sift.async_maintenance = off
sift.col_auto_merge = off
sift.test_col_floor_sbs = 4"
node_start fl || bail "start failed"
sql fl "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE fl (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO fl SELECT g, g % 7, g % 1000, repeat('x', 1000) FROM generate_series(1, 40000) g" \
	"CREATE INDEX fl_idx ON fl USING sift_col (id, grp, amount) WITH (write_buffer_limit = 64)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) fl" >/dev/null || bail "setup failed"

answer() { sql "${2:-fl}" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT grp, count(*), sum(amount) FROM fl GROUP BY grp) x" | tail -1; }
ctidq() { sql "${2:-fl}" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "SELECT count(*) || ' ' || sum(amount) FROM fl WHERE ctid >= '(800,1)' AND ctid < '(3000,1)'" | tail -1; }
# the immutable segments: generation, rows, units
segs() { sql "${1:-fl}" "SELECT string_agg(gen || ':' || rows || ':' || units, ' ' ORDER BY gen) FROM sift.col_segments('fl_idx') WHERE kind = 'immutable'"; }
newest_units() { sql fl "SELECT units FROM sift.col_segments('fl_idx') WHERE kind = 'immutable' ORDER BY gen DESC LIMIT 1"; }
verified() { test -z "$(sql "${1:-fl}" "SELECT sift.col_verify('fl_idx', true)" 2>&1)"; }
round() { sql fl "UPDATE fl SET amount = amount + 1 WHERE id % 10 = $1" "VACUUM fl" >/dev/null; }
panics() { ! sql fl "$@" >/dev/null 2>&1; }
restarted() { for _ in $(seq 300); do [[ -f $(dir fl)/data/postmaster.pid ]] || break; sleep 0.1; done; node_start fl; }
same() { [[ $1 == "$2" ]] || diag "got:"$'\n'"$1"$'\n'"want:"$'\n'"$2"; [[ $1 == "$2" ]]; }

round 0
check "a promotion over most superblocks is a few units cut by TID range, not a unit for each" test "$(newest_units)" -le 3
HEAP=$(answer off)
check "the index verifies and the answers are the heap's, serial and by ctid range" test "$(verified && echo ok)|$(answer on)|$(ctidq on)" == "ok|$HEAP|$(ctidq off)"

# seven more promotions: the eighth is the tier merge of the overlays, which stays sparse
for k in 1 2 3 4 5 6; do round "$k"; done
sql fl "CHECKPOINT" >/dev/null
BEFORE=$(segs)
HEAP=$(answer off)
SIFT_TMP=$(dir fl) leak_expect 2 'PANIC:'
SIFT_TMP=$(dir fl) leak_expect 2 'terminated by signal 6'
check "the tier merge of the overlays PANICs once its segment is written" panics "SET sift.test_col_merge_panic = on" "UPDATE fl SET amount = amount + 1 WHERE id % 10 = 7" "VACUUM fl"
check "the server restarts through crash recovery" restarted
check "the overlays are as they were, the index verifies and the answers are the heap's" same "$(verified && echo ok)|$(answer on)" "ok|$(answer off)"
round 7
check "the tier merge, asked again, completes: fewer segments, the merged overlay in few units" \
	test "$(sql fl "SELECT count(*) < 9 FROM sift.col_segments('fl_idx') WHERE kind = 'immutable'")|$(sql fl "SELECT max(units) <= 4 FROM sift.col_segments('fl_idx') WHERE kind = 'immutable' AND rows < 20000")" == "t|t"
check "and the index verifies and answers the heap's" test "$(verified && echo ok)|$(answer on)|$(ctidq on)" == "ok|$(answer off)|$(ctidq off)"

# the merge of all of them: the base is cut by superblock, the overlays by TID range and by superblock
sql fl "SET sift.col_unit_floor = 0" "UPDATE fl SET amount = amount + 1 WHERE id % 10 = 8" "VACUUM fl" >/dev/null
sql fl "CHECKPOINT" >/dev/null
BEFORE=$(segs)
check "the merge of all PANICs once its segment is written" panics "SET sift.test_col_merge_panic = on" "SELECT sift.col_merge('fl_idx', true)"
check "the server restarts" restarted
check "the segments are as they were, the index verifies and the answers are the heap's" same "$(segs)|$(verified && echo ok)|$(answer on)" "$BEFORE|ok|$(answer off)"
check "the merge of all, asked again, completes into a unit for each superblock" \
	test "$(sql fl "SELECT sift.col_merge('fl_idx', true) >= 0")|$(sql fl "SELECT count(*) || ' ' || sum(units) = (SELECT '1 ' || ceil(pg_relation_size('fl') / 8192.0 / 256)) FROM sift.col_segments('fl_idx') WHERE kind = 'immutable'")" == "t|t"
check "the index verifies and answers the heap's after it" test "$(verified && echo ok)|$(answer on)|$(ctidq on)" == "ok|$(answer off)|$(ctidq off)"

# a hot standby replays the promotions and merges
round 1
node_clone fl standby 27 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"
for k in 2 3 4 5 6 7; do round "$k"; done
sql fl "SELECT sift.col_merge('fl_idx', true) >= 0" "UPDATE fl SET amount = amount + 1 WHERE id % 10 = 9" "VACUUM fl" >/dev/null
catchup fl standby
check "the standby's segments are the primary's" same "$(segs standby)" "$(segs fl)"
check "the standby verifies and answers as the primary's heap does, a ctid range included" \
	same "$(verified standby && echo ok)|$(answer on standby)|$(ctidq on standby)" "ok|$(answer off)|$(ctidq off)"
node_promote standby
for _ in $(seq 100); do [[ $(sql standby "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql standby "UPDATE fl SET amount = amount + 1 WHERE id % 11 = 0" "VACUUM fl" "SELECT sift.col_merge('fl_idx', true) >= 0" >/dev/null
check "the promoted standby takes writes and its index verifies and answers its heap's" same "$(verified standby && echo ok)|$(answer on standby)" "ok|$(answer off standby)"
node_stop standby

# the index the last release wrote
if ((PG_MAJOR != 18)); then
	skip 5 "the released libraries build on PostgreSQL 18 only"
elif ! git -C "$REPO" rev-parse --verify -q "v3.3.0^{commit}" >/dev/null; then
	skip 5 "released library source v3.3.0 is unavailable"
else
	ref=v3.3.0
	newroot=$ROOT
	build_old "$ref"
	oldroot=$OLDROOT
	node_init old 28 "autovacuum = off
sift.async_maintenance = off"
	use_root old "$oldroot"
	node_start old || bail "cannot start $ref"
	sql old "CREATE EXTENSION pg_sift" "CREATE TABLE fl (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
		"INSERT INTO fl SELECT g, g % 7, g % 1000, repeat('x', 1000) FROM generate_series(1, 40000) g" \
		"CREATE INDEX fl_idx ON fl USING sift_col (id, grp, amount) WITH (write_buffer_limit = 64)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) fl" \
		"UPDATE fl SET amount = amount + 1 WHERE id % 10 = 0" "VACUUM fl" "SELECT sift.col_promote('fl_idx')" >/dev/null || bail "$ref setup"
	WANT=$(answer off old)
	OLDSEGS=$(segs old)
	node_stop old
	use_root old "$newroot"
	node_conf old "sift.test_col_floor_sbs = 4"
	node_start old || bail "cannot start the new library"
	sql old "ALTER EXTENSION pg_sift UPDATE" >/dev/null || bail "SQL update"
	check "this library reads what $ref wrote: same segments, verifies, answers the heap's" same "$(segs old)|$(verified old && echo ok)|$(answer on old)" "$OLDSEGS|ok|$WANT"
	sql old "SET sift.col_auto_merge = off" "UPDATE fl SET amount = amount + 1 WHERE id % 10 = 1" "VACUUM fl" "SELECT sift.col_promote('fl_idx')" >/dev/null
	check "its promotion next to those segments is a few units cut by TID range" test "$(sql old "SELECT units FROM sift.col_segments('fl_idx') WHERE kind = 'immutable' ORDER BY gen DESC LIMIT 1")" -le 3
	check "the index verifies and answers the heap's with both kinds of segment, a ctid range included" same "$(verified old && echo ok)|$(answer on old)|$(ctidq on old)" "ok|$(answer off old)|$(ctidq off old)"
	check "the merge of all of them is a unit for each superblock again" \
		test "$(sql old "SELECT sift.col_merge('fl_idx', true) >= 0")|$(sql old "SELECT sum(units) = ceil(pg_relation_size('fl') / 8192.0 / 256) FROM sift.col_segments('fl_idx') WHERE kind = 'immutable'")" == "t|t"
	check "and the index verifies and answers the heap's" same "$(verified old && echo ok)|$(answer on old)" "ok|$(answer off old)"
	node_stop old
fi
echo "1..$NTEST"
finish
