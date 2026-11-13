#!/usr/bin/env bash
# The oracle gate of the scope index (test/fuzz/scope_oracle.py): random workloads on the facts and the pairs of a scope index (one path, soft deleted pairs, two paths) whose expected answer comes from stock PostgreSQL
# only. Four nodes: the primary (pg_sift, the scope index, wal_level logical), a physical hot standby of it, a STOCK node that never loads pg_sift and holds the plain tables fed by logical replication of the base
# tables (the oracle), and a node that restores a pg_dump of the primary. After each step the battery of reads (one key, sets of keys counted once, per pair, every key, top-N, EXISTS) is compared row for row: scope
# units, planned and plain plans on the primary in one REPEATABLE READ snapshot, the stock node, the standby and the restored database. Reads inside the editing transaction (own command ids, savepoints), old
# REPEATABLE READ and SERIALIZABLE snapshots across committed edits, a writer that stays open across VACUUM, promotion and merge, windows of pgbench writers with readers on the primary and the standby and a CREATE INDEX
# CONCURRENTLY inside, REINDEX (also CONCURRENTLY), and sift.col_scope_verify on the primary, the standby and the restore. Odd seeds are the walk profile: wide rows, rows dated by member, units cut by member, the latest-N reads walk the superblocks by their zones, and one session alternates sift.agg_scope off and on (a latest-N read of a
# scope index after a join read of the same keys returned older rows before the fix that stamps the walk with the source of its zones). Quick tier: seeds 1-3 of 4 steps on 20,000 rows; full: seeds 1-6 of 8 steps on 60,000.
# SIFT_ORACLE_SEEDS / SIFT_ORACLE_ROWS / SIFT_ORACLE_STEPS / SIFT_ORACLE_WINDOWS override; SIFT_ORACLE_OFFSET moves the four nodes (ports SIFT_PORT + offset .. + 3). Not covered: crash recovery
# (col_scope_crash.sh), cross-major pg_upgrade (col_scope_upgrade.sh).
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_ORACLE_SEEDS 1-6 1-3)
ROWS=$(knob SIFT_ORACLE_ROWS 60000 20000)
STEPS=$(knob SIFT_ORACLE_STEPS 8 4)
WINDOWS=$(knob SIFT_ORACLE_WINDOWS 1 1)
BASE=${SIFT_ORACLE_OFFSET:-80}

REPL="wal_level = logical
max_wal_senders = 8
max_replication_slots = 16
max_logical_replication_workers = 8
max_worker_processes = 24
wal_keep_size = 512MB
max_wal_size = 1GB
shared_buffers = 128MB
max_locks_per_transaction = 256
autovacuum = off
max_connections = 100"
node_init ora_a "$BASE" "$REPL
sift.scope_max_multiplicity = 8"
node_start ora_a || bail "primary start failed"
node_init ora_b $((BASE + 2)) "$REPL"
for k in '^sift[.]' '^extension_control_path' '^dynamic_library_path'; do sed_i "/$k/d" "$(dir ora_b)/data/postgresql.conf"; done
node_start ora_b || bail "stock node start failed"
node_init ora_r $((BASE + 3)) "$REPL
sift.scope_max_multiplicity = 64"
node_start ora_r || bail "restore node start failed"
sql ora_a "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
node_clone ora_a ora_s $((BASE + 1)) "hot_standby_feedback = on
max_standby_streaming_delay = 30s
$REPL"
node_start ora_s || bail "standby start failed"
plan 4
check "the stock node runs without pg_sift (no preload, no extension)" test "$(sql ora_b "SELECT count(*) FROM pg_extension WHERE extname = 'pg_sift'")|$(sql ora_b "SHOW shared_preload_libraries")|$(sql ora_b "SHOW session_preload_libraries")" == "0||"

S() { echo "$(dir "$1")/sock:$(port "$1")"; }
out=$(python3 -I "$REPO/test/fuzz/scope_oracle.py" --primary "$(S ora_a)" --standby "$(S ora_s)" --stock "$(S ora_b)" --restore "$(S ora_r)" \
	--out "$(dir ora_a)/oracle" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" --windows "$WINDOWS" --toggle-every "${SIFT_ORACLE_TOGGLE:-1}" 2>&1)
rc=$?
diag "$(grep -E '^(FAIL|ok:|seed |stats|scope reads)' <<<"$out" | cut -c1-6000)"
check "every read of the scope index is the answer of stock PostgreSQL, row for row, in every seed ($SEEDS)" test "$rc" == 0
# the gate must catch a mutant of the tracking: a pair deleted while its journal writes nothing leaves the stored entries behind
out=$(SIFT_ORACLE_SET="SET sift.test_no_track = on;" python3 -I "$REPO/test/fuzz/scope_oracle.py" --primary "$(S ora_a)" --standby "$(S ora_s)" --stock "$(S ora_b)" --restore "$(S ora_r)" \
	--out "$(dir ora_a)/oracle-mutant" --seeds 13 --rows 20000 --steps 6 2>&1)
diag "$(grep -E '^FAIL' <<<"$out" | cut -c1-300)"
check "the gate catches the mutant sift.test_no_track (a wrong answer or a key sift.col_scope_verify reports)" test "$(grep -cE 'FAIL.*(WRONG ANSWER|col_scope_verify)' <<<"$out")" -ge 1
check "the stock node never loaded pg_sift while it was the oracle" test "$(grep -c 'pg_sift' "$(dir ora_b)/postgres.log")" == 0
finish
