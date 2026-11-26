#!/usr/bin/env bash
# tap: pooled
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# What a concurrent build may hold and whom it may make wait, with PostgreSQL's own builds (a btree and a GIN CREATE INDEX CONCURRENTLY, and REINDEX CONCURRENTLY of them) as the reference, and the
# same assertions for sift_col (plain, tracking changes, with a scope over a table of pairs, REINDEX CONCURRENTLY). The build is held in every phase it has (an advisory lock a session holds: the
# expression of the btree and the GIN index, sift.test_phase_gate for sift_col) and in each:
#   - the build backend holds no lock on the table or on the pairs table that conflicts with a writer (nothing stronger than ShareUpdateExclusiveLock),
#   - INSERT, UPDATE and DELETE of the table and of the pairs table from other sessions complete, also while a VACUUM waits for the build,
#   - a build killed (terminate or cancel) leaves an INVALID index that a plain DROP INDEX CONCURRENTLY IF EXISTS removes.
# A writer that stays open while the build runs holds the build at the points where it waits for the writers; the build asks for no lock then (nothing queues behind a request that new writers would
# wait for), and the new writers complete. After the build the answer of the index is the heap's, for the rows written before, during and after each phase.
# A scope over a relation of pairs that has no journal yet is built too, with neither the table nor the pairs having a journal: the journal is made inside the build, and the writers of both tables must
# complete while it is made (gate 6), as they do when the journal exists.
# SIFT_CL_KINDS: the kinds to run (default all).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

KINDS=${SIFT_CL_KINDS:-btree gin sift_plain sift_track sift_scope sift_scope_new_journal reindex_btree reindex_gin reindex_sift_scope}
PT=pairs
# Format 2: the indexes a concurrent build of the release library makes
node_init cl 71 "autovacuum = off
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
sift.test_create_format1 = off"
node_start cl || bail "start failed"
D=$(dir cl)
FG=7101			# the advisory lock the expression of the btree and GIN indexes waits for in the build
SG=7200			# sift.test_phase_gate of the build of sift_col: the phase gates are SG + the phase (2 scan, 3 write, 4 tail, 5 fill, 6 the journal of a relation of pairs that has none, while the build holds what it holds to make it)
STRONG="'ShareLock', 'ShareRowExclusiveLock', 'ExclusiveLock', 'AccessExclusiveLock'"
sql cl "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE FUNCTION gate_i(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS \$\$ BEGIN IF current_setting('application_name') = 'build' THEN PERFORM pg_advisory_lock($FG); PERFORM pg_advisory_unlock($FG); END IF; RETURN \$1; END \$\$" \
	"CREATE FUNCTION gate_a(int[]) RETURNS int[] LANGUAGE plpgsql IMMUTABLE AS \$\$ BEGIN IF current_setting('application_name') = 'build' THEN PERFORM pg_advisory_lock($FG); PERFORM pg_advisory_unlock($FG); END IF; RETURN \$1; END \$\$" \
	"CREATE TABLE pairs (m int NOT NULL, t int NOT NULL, PRIMARY KEY (m, t))" \
	"INSERT INTO pairs SELECT m, 1 + (m * 7 + j * 5) % 12 FROM generate_series(1, 400) m, generate_series(0, 2) j WHERE j < 1 + m % 3 ON CONFLICT DO NOTHING" \
	"CREATE INDEX CONCURRENTLY pairs_j ON pairs USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE pairs2 AS SELECT * FROM pairs" "ALTER TABLE pairs2 ADD PRIMARY KEY (m, t)" \
	"CREATE TABLE t (id int PRIMARY KEY, m int, d timestamptz, v int, a int[], pad text)" \
	"INSERT INTO t SELECT i, 1 + i % 400, timestamptz '2026-01-01' + i * interval '1 minute', i % 10, ARRAY[i % 7, i % 11], 'x' FROM generate_series(1, 20000) i" \
	"VACUUM (FREEZE, ANALYZE) t, pairs, pairs2" >/dev/null || bail "setup failed"

declare -A GATEPID
gate_hold() { # KEY: a session holds the advisory lock KEY until gate_open KEY
	rm -f "$D/g$1".*
	{ echo "SELECT pg_advisory_lock($1);"; echo "\\! touch '$D/g$1.held'"; echo "\\! while [ ! -e '$D/g$1.open' ]; do sleep 0.05; done"; } | PGAPPNAME=gate sql cl >/dev/null 2>&1 &
	GATEPID[$1]=$!
	wait_for "[[ -e $D/g$1.held ]]" 60 || bail "gate $1 was not taken"
}
gate_open() { touch "$D/g$1.open"; wait "${GATEPID[$1]}"; }
at_gate() { [[ $(sql cl "SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid) WHERE a.application_name = 'build' AND l.locktype = 'advisory' AND l.objid = $1 AND NOT l.granted") == 1 ]]; }
build_pid() { sql cl "SELECT pid FROM pg_stat_activity WHERE application_name = 'build' AND backend_type = 'client backend'"; }
build_gone() { [[ -z $(build_pid) ]]; }
blocked() { [[ $(sql cl "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'build' AND wait_event_type = 'Lock' AND wait_event <> 'advisory'") == 1 ]]; }
build_bg() { # [SIFT_GATE] SQL: a build session (application_name build), serial, with the phase gates of sift_col when asked
	local sets=()
	[[ $1 != sift ]] || sets=("SET sift.test_phase_gate = $SG")
	shift
	PGAPPNAME=build sql cl "SET max_parallel_maintenance_workers = 0" "${sets[@]}" "$@" >"$D/build.out" 2>&1 &
	BUILD=$!
}
wr() { PGAPPNAME=writer sql cl "SET lock_timeout = '20s'" "$@" >"$D/wr.out" 2>&1; }

SEQ=0
writers() { # the three statements of a writer on the table, with ids no other round touches
	SEQ=$((SEQ + 1))
	wr "INSERT INTO t SELECT $SEQ * 100000 + g, 1 + g % 400, timestamptz '2026-03-01', g % 10, ARRAY[g % 7], 'w' FROM generate_series(1, 30) g" \
		"UPDATE t SET v = v + 1, m = 1 + (m + $SEQ) % 400 WHERE id % 211 = $SEQ AND id < 20000" "DELETE FROM t WHERE id = 1000 + $SEQ * 7"
}
pair_writers() {
	wr "INSERT INTO $PT VALUES (1 + $SEQ, 5) ON CONFLICT DO NOTHING" "DELETE FROM $PT WHERE m = 10 + $SEQ AND t = 5" \
		"UPDATE $PT SET t = 13 + $SEQ WHERE m = 30 + $SEQ AND t = (SELECT min(t) FROM $PT WHERE m = 30 + $SEQ)"
}
held_modes() { sql cl "SELECT coalesce(string_agg(DISTINCT mode, ',' ORDER BY mode), '-') FROM pg_locks WHERE pid = $BPID AND locktype = 'relation' AND granted AND relation IN ('t'::regclass, 'pairs'::regclass, 'pairs2'::regclass)"; }
strong_held() { sql cl "SELECT string_agg(DISTINCT mode, ',') FROM pg_locks WHERE pid = $BPID AND locktype = 'relation' AND granted AND mode IN ($STRONG) AND relation IN ('t'::regclass, 'pairs'::regclass, 'pairs2'::regclass)"; }
asking() { sql cl "SELECT count(*) FROM pg_locks WHERE pid = $BPID AND locktype = 'relation' AND NOT granted"; }
vacuum_waits() { [[ $(sql cl "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'vacuumer' AND wait_event_type = 'Lock'") == 1 ]]; }
invalid_left() { [[ $(sql cl "SELECT indisvalid FROM pg_index WHERE indexrelid = to_regclass('$1')") == f ]]; }
dropped() { sql cl "DROP INDEX CONCURRENTLY IF EXISTS $1" >/dev/null && [[ -z $(sql cl "SELECT to_regclass('$1')") ]]; }

ANSWER="SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM (SELECT m % 13 g, count(*) c, sum(v) s FROM t GROUP BY 1) x"
SCOPE_READ="SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM (SELECT %s FROM t e WHERE sift.in_scope('%s'::regclass, 'tenant', e.m, 5)) x"
join_read() { echo "SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM (SELECT count(*), sum(v), sum(id) FROM t e WHERE e.m IN (SELECT m FROM $PT WHERE t = 5)) x"; }
same_answer() { # INDEX KIND: the index answers as the heap does
	local idx=$1 q a b
	q=$(printf "$SCOPE_READ" "count(*), sum(v), sum(id)" "$idx")
	if [[ $2 == sift_scope ]]; then
		a=$(sql cl "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "$q")
		b=$(sql cl "SET sift.enable_agg = off" "$(join_read)")
		[[ $a == "$b" ]] || return 1
	fi
	[[ $2 == btree || $2 == gin ]] && return 0
	[[ $(sql cl "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "$ANSWER") == "$(sql cl "SET sift.enable_agg = off" "$ANSWER")" ]]
}
stored() { # INDEX: the scope is read from the summary the build filled and stamped, not from the join
	sift_explain=$(sql cl "SET sift.enable_agg = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), sum(v) FROM t e WHERE e.m IN (SELECT m FROM $PT WHERE t = 5)")
	grep -q "Index: $1" <<<"$sift_explain" && grep -q 'Scope Read: summary' <<<"$sift_explain"
}
journal_whole() { # the journal of pairs2 made by the build: valid, ready, and the heap's rows are in it
	local j
	j=$(sql cl "SELECT c.relname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 'pairs2'::regclass AND c.relname LIKE '%sift_journal' AND i.indisvalid AND i.indisready")
	[[ -n $j ]] && [[ -z $(sql cl "SELECT sift.col_verify('$j', true)" 2>&1) ]]
}
journal_in_use() { # INDEX: the journal of the pairs table the scope index depends on: the drop fails on the dependency without waiting, and the journal stays
	local j out
	j=$(sql cl "SELECT d.refobjid::regclass FROM pg_depend d JOIN pg_class c ON c.oid = d.refobjid JOIN pg_am a ON a.oid = c.relam WHERE d.classid = 'pg_class'::regclass AND d.objid = '$1'::regclass AND a.amname = 'sift_col' AND c.oid IN (SELECT indexrelid FROM pg_index WHERE indrelid = '$PT'::regclass)")
	[[ -n $j ]] || return 1
	out=$(sql cl "SET lock_timeout = '3s'" "DROP INDEX CONCURRENTLY $j" 2>&1) # timeout-ok: the drop fails on the dependency before it asks for a lock, so the timeout is never reached when the product is right
	[[ $out == *depend* && -n $(sql cl "SELECT to_regclass('$j')") ]]
}
verifies() { [[ -z $(sql cl "SELECT sift.col_verify('$1', true)" 2>&1) ]]; }

# run_kind KIND INDEX CREATE-SQL MODE(create|reindex) GATE(fn|sift) PHASES...: one build held at each phase in turn (the asserts), then to the end
run_kind() {
	local kind=$1 idx=$2 create=$3 mode=$4 gate=$5 ph key stmt shape=${1#reindex_}; [[ $shape != sift_scope_new_journal ]] || shape=sift_scope
	shift 5
	local phases=("$@")
	[[ $mode == reindex ]] && stmt="REINDEX INDEX CONCURRENTLY $idx" || stmt=$create
	[[ $mode == reindex ]] && { sql cl "$create" >/dev/null || bail "$kind: setup failed"; }
	for ph in "${phases[@]}"; do gate_hold "$([[ $gate == fn ]] && echo $FG || echo $((SG + ph)))"; done
	build_bg "$gate" "$stmt"
	for ph in "${phases[@]}"; do
		key=$([[ $gate == fn ]] && echo $FG || echo $((SG + ph)))
		wait_for "at_gate $key" 120 || bail "$kind: the build did not reach phase $ph ($(sql cl "SELECT wait_event_type, wait_event, state FROM pg_stat_activity WHERE application_name = 'build'") $(head -n 3 "$D/build.out"))"
		BPID=$(build_pid)
		check "$kind phase $ph: the build holds nothing stronger than ShareUpdateExclusiveLock on the tables ($(held_modes))" test -z "$(strong_held)"
		writers
		check "$kind phase $ph: INSERT, UPDATE and DELETE of the table complete" test $? == 0
		pair_writers
		check "$kind phase $ph: INSERT, UPDATE and DELETE of the pairs table complete" test $? == 0
		if [[ $ph == "${phases[0]}" ]]; then
			PGAPPNAME=vacuumer sql cl "VACUUM t" >"$D/vac.out" 2>&1 &
			VAC=$!
			wait_for vacuum_waits 60
			check "$kind: a VACUUM of the table waits for the build" vacuum_waits
		fi
		writers
		check "$kind phase $ph: writers complete while a VACUUM waits for the build" test $? == 0
		check "$kind phase $ph: the build asks for no lock" test "$(asking)" == 0
		gate_open "$key"
	done
	wait "$BUILD"; local rc=$?
	wait "$VAC"
	check "$kind: the build ends" test "$rc" == 0
	diag "$(head -n 3 "$D/build.out")"
	check "$kind: the index is valid" test "$(sql cl "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid = '$idx'::regclass")" == t
	writers
	check "$kind: writers complete after the build" test $? == 0
	pair_writers
	check "$kind: the writers of the pairs table complete after the build" test $? == 0
	check "$kind: the index answers as the heap does, for the rows written before, during and after each phase" same_answer "$idx" "$shape"
	[[ $shape != sift* ]] || check "$kind: the index verifies" verifies "$idx"
	[[ $shape != sift_scope ]] || check "$kind: the scope is read from its summary as soon as the build ends" stored "$idx"
	[[ $shape != sift_scope ]] || check "$kind: DROP INDEX CONCURRENTLY of the journal the scope index uses is refused at once, holding no lock" journal_in_use "$idx"
	[[ $PT != pairs2 ]] || check "$kind: the journal the build made is valid, verifies against the pairs written while it was filled, and no scope index waits for it" journal_whole
}

# kill_kind KIND INDEX CREATE-SQL GATE HOW(terminate|cancel) PHASE: the build is killed in a phase; the INVALID index is dropped by a plain DROP INDEX CONCURRENTLY IF EXISTS
kill_kind() {
	local kind=$1 idx=$2 create=$3 gate=$4 how=$5 ph=$6 key
	key=$([[ $gate == fn ]] && echo $FG || echo $((SG + ph)))
	gate_hold "$key"
	build_bg "$gate" "$create"
	wait_for "at_gate $key" 120 || bail "$kind: the build did not reach phase $ph"
	BPID=$(build_pid)
	sql cl "SELECT pg_${how}_backend($BPID)" >/dev/null
	wait "$BUILD"
	gate_open "$key"
	wait_for build_gone 60
	check "$kind $how in phase $ph: an INVALID index is left" invalid_left "$idx"
	writers
	check "$kind $how in phase $ph: writers complete" test $? == 0
	check "$kind $how in phase $ph: DROP INDEX CONCURRENTLY IF EXISTS removes it" dropped "$idx"
	[[ $gate != sift ]] || { sql cl "VACUUM t, pairs" >/dev/null
		check "$kind $how in phase $ph: the other sift index of the tables verifies and no relation of the dropped index is left" \
			test -z "$(sql cl "SELECT sift.col_verify('pairs_j', true)" 2>&1)" -a "$(sql cl "SELECT count(*) FROM pg_class WHERE relname LIKE '$idx%'")" == 0; }
	[[ $PT != pairs2 ]] || check "$kind $how in phase $ph: the journal the build made is dropped with its index" \
		test "$(sql cl "SELECT count(*) FROM pg_class WHERE relname LIKE 'pairs2%sift_journal'")" == 0
	check "$kind $how in phase $ph: no lock is left on the tables" test "$(sql cl "SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND relation IN ('t'::regclass, 'pairs'::regclass, 'pairs2'::regclass) AND mode IN ($STRONG) AND granted")" == 0
}

# open_writer KIND INDEX CREATE-SQL GATE: a writer of both tables stays open from the first phase; the build waits for it where it waits for writers, asks for no lock, and the new writers complete
open_writer() {
	local kind=$1 idx=$2 create=$3 gate=$4 ph=$5 key
	key=$([[ $gate == fn ]] && echo $FG || echo $((SG + ph)))
	gate_hold "$key"
	build_bg "$gate" "$create"
	wait_for "at_gate $key" 120 || bail "$kind: the build did not reach phase $ph"
	SEQ=$((SEQ + 1)); rm -f "$D/w.open" "$D/w.held"
	{ echo "BEGIN;"; echo "INSERT INTO t VALUES ($((900000000 + SEQ)), 1, now(), 1, ARRAY[1], 'open');"; echo "INSERT INTO $PT VALUES (399, 99);"; echo "\\! touch '$D/w.held'"
		echo "\\! while [ ! -e '$D/w.open' ]; do sleep 0.05; done"; echo "COMMIT;"; } | PGAPPNAME=openwriter sql cl >"$D/open.out" 2>&1 &
	OPEN=$!
	wait_for "[[ -e $D/w.held ]]" 60 || bail "$kind: the open writer did not start"
	gate_open "$key"
	wait_for blocked 120 || bail "$kind: the build did not wait for the open writer ($(cat "$D/build.out" | head -n 2))"
	BPID=$(build_pid)
	check "$kind: waiting for an open writer, the build asks for no lock a new writer would queue behind" test "$(asking)" == 0
	writers
	check "$kind: waiting for an open writer, new writers of the table complete" test $? == 0
	pair_writers
	check "$kind: waiting for an open writer, new writers of the pairs table complete" test $? == 0
	check "$kind: waiting for an open writer, the build holds nothing stronger than ShareUpdateExclusiveLock ($(held_modes))" test -z "$(strong_held)"
	touch "$D/w.open"
	wait "$OPEN"
	wait "$BUILD"; local rc=$?
	check "$kind: the build ends once the writer commits" test "$rc" == 0
	check "$kind: the index is valid" test "$(sql cl "SELECT indisvalid FROM pg_index WHERE indexrelid = '$idx'::regclass")" == t
	sql cl "DELETE FROM $PT WHERE t = 99" >/dev/null
}

for kind in $KINDS; do
	PT=pairs
	case $kind in
		btree)
			C="CREATE INDEX CONCURRENTLY k_ix ON t (gate_i(v))" ; G=fn ; P=(1) ;;
		gin)
			C="CREATE INDEX CONCURRENTLY k_ix ON t USING gin (gate_a(a))" ; G=fn ; P=(1) ;;
		sift_plain)
			C="CREATE INDEX CONCURRENTLY k_ix ON t USING sift_col (id, m, d, v) WITH (track_changes = 'off')" ; G=sift ; P=(2 3 4) ;;
		sift_track)
			C="CREATE INDEX CONCURRENTLY k_ix ON t USING sift_col (id, m, d, v)" ; G=sift ; P=(2 3 4) ;;
		sift_scope)
			C="CREATE INDEX CONCURRENTLY k_ix ON t USING sift_col (id, m, d, v) WITH (scope = 'tenant: m -> pairs(m, t) max_multiplicity(8)')" ; G=sift ; P=(2 3 4 5) ;;
		sift_scope_new_journal)
			C="CREATE INDEX CONCURRENTLY k_ix ON t USING sift_col (id, m, d, v) WITH (scope = 'tenant: m -> pairs2(m, t) max_multiplicity(8)')" ; G=sift ; P=(2 3 4 6 5) ; PT=pairs2 ;;
		reindex_btree)
			C="CREATE INDEX k_ix ON t (gate_i(v))" ; G=fn ; P=(1) ;;
		reindex_gin)
			C="CREATE INDEX k_ix ON t USING gin (gate_a(a))" ; G=fn ; P=(1) ;;
		reindex_sift_scope)
			C="CREATE INDEX k_ix ON t USING sift_col (id, m, d, v) WITH (scope = 'tenant: m -> pairs(m, t) max_multiplicity(8)')" ; G=sift ; P=(2 3 4 5) ;;
		*) bail "unknown kind $kind" ;;
	esac
	if [[ $kind == reindex_* ]]; then
		run_kind "$kind" k_ix "$C" reindex "$G" "${P[@]}"
		sql cl "DROP INDEX k_ix" >/dev/null
		continue
	fi
	run_kind "$kind" k_ix "$C" create "$G" "${P[@]}"
	sql cl "DROP INDEX k_ix" >/dev/null
	kill_kind "$kind" k_ix "$C" "$G" terminate "${P[${#P[@]} - 1]}"
	kill_kind "$kind" k_ix "$C" "$G" cancel "${P[0]}"
	[[ $kind != sift_scope_new_journal ]] || kill_kind "$kind" k_ix "$C" "$G" cancel 6
	open_writer "$kind" k_ix "$C" "$G" "${P[0]}"
	sql cl "DROP INDEX k_ix" >/dev/null
done
done_testing
finish
