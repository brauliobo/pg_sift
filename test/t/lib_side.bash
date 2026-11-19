# Helpers of the tests of a scope index of one path whose entity segments hold a scope sidecar (col/side.h; docs/COL-SCOPE.md). A scope read takes the runs of the sidecar for the units they stand for, or
# reads the units by reference, or runs the join; its answer is the join's in every state. The fixture has a sidecar only when the index is cut by its member and the units are small:
# 400 rows of a member are 3 units of 150 rows, and an entity segment needs 16 units.
#
#   SIDE_SQL             the function that sends statements to a node or a database (default sql), as TWOHOP_SQL
#   SIDE_CONF            node settings: units of 150 rows
#   SIDE_WITH            the options of the index that keep one member's rows together, for the scope declared in SIDE_SCOPE
#   side_setup NODE, side_facts NODE FROM TO   the fixture: tenant_scope and its journal, events
#   side_read KEY / side_join KEY   the read of a key of tenant_scope over events by kind (the shape whose runs answer from their cubes: an ungrouped total is answered from the pre-aggregates of the units) and its join
#   side_runs NODE QUERY       the runs of the sidecar the entity arm took for the read QUERY (0: none)
#   side_arm NODE QUERY        entity when the read of QUERY scanned the entity rows, else the reason of its fallback
#   side_same NODE READ JOIN   the answer of the read is the join's
#   side_format NODE INDEX     the highest column format minor of the segments of the index (53: a segment has a sidecar)
#   side_clean NODE INDEX      the index verifies and no scope key differs from the join

SIDE_CONF="sift.col_unit_rows = 150"
SIDE_SCOPE="tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)"
SIDE_WITH="key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = '$SIDE_SCOPE'"

side_read() { echo "SELECT kind, count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, $1) GROUP BY kind ORDER BY kind"; }
side_join() { echo "SELECT kind, count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = $1) GROUP BY kind ORDER BY kind"; }
# side_setup NODE [nojournal]: the extensions, the pairs of 60 members (tenant 1 has 50 of them, tenant 2 has 20, tenant 3 has 2) with their journal (left to the first scope index with nojournal), and the
# table of the facts, with no index
side_setup() {
	sql "$1" "CREATE EXTENSION IF NOT EXISTS pg_sift" "CREATE EXTENSION IF NOT EXISTS pg_sift_test" \
		"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
		"INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 60) m WHERE m % 6 < 5" "INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 60) m WHERE m % 3 = 0" \
		"INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 60) m WHERE m IN (7, 8)" \
		"$([[ ${2:-} == nojournal ]] || echo "CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')")" \
		"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" >/dev/null
}
# side_facts NODE FROM TO: the facts FROM..TO, 400 rows for each of the 60 members over 24,000
side_facts() { sql "$1" "INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series($2, $3) i" >/dev/null; }
side_explain() {
	local n=$1 q=$2
	${SIDE_SQL:-sql} "$n" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SET jit = off" "SET sift.test_scope_entity = on" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $q"
}
side_runs() { local r; r=$(side_explain "$1" "$2" | sed -n 's/^ *Scope Sidecar Runs: //p' | head -n 1); echo "${r:-0}"; }
side_arm() {
	local p why
	p=$(side_explain "$1" "$2")
	why=$(sed -n 's/^ *Scope Fallback: //p' <<<"$p" | head -n 1)
	if [[ -n $why ]]; then echo "$why"; elif grep -q 'Scope Units' <<<"$p"; then echo entity; else echo none; fi
}
side_same() { [[ $(${SIDE_SQL:-sql} "$1" "SET sift.agg_force = on" "$2") == "$(${SIDE_SQL:-sql} "$1" "SET sift.enable_agg = off" "$3")" ]]; }
side_format() { ${SIDE_SQL:-sql} "$1" "SELECT coalesce(max(format), 0) FROM sift._test_col_formats('$2')" | tail -n 1; }
side_clean() {
	local Q=${SIDE_SQL:-sql}
	[[ -z $($Q "$1" "SELECT sift.col_verify('$2', true)" 2>&1) && $($Q "$1" "SELECT count(*) FROM sift.col_scope_verify('$2', 1000) WHERE problem <> 'unverified'" | tail -n 1) == 0 ]]
}

# A build held after it has read the pairs and taken the stamp, before it writes the sidecars (sift.test_phase_gate + SIDE_GATE): a session of the node holds the advisory lock of that phase until
# side_gate_open, the build is a session with application_name build that sets sift.test_phase_gate = SIDE_GATE_BASE
#   side_gate_hold NODE      take the lock of the phase
#   side_gate_reached NODE   the build waits at it
#   side_gate_open NODE      let it go
SIDE_GATE_BASE=7400
SIDE_GATE=10
side_gate_hold() {
	local d
	d=$(dir "$1")
	rm -f "$d"/gate.*
	{ echo "SELECT pg_advisory_lock($((SIDE_GATE_BASE + SIDE_GATE)));"; echo "\\! touch '$d/gate.held'"; echo "\\! while [ ! -e '$d/gate.open' ]; do sleep 0.05; done"; } | sql "$1" >/dev/null 2>&1 &
	SIDE_GATE_PID=$!
	wait_for "[[ -e $d/gate.held ]]" 60 || bail "the gate was not taken"
}
side_gate_reached() { [[ $(sql "$1" "SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid) WHERE a.application_name = 'build' AND l.locktype = 'advisory' AND l.objid = $((SIDE_GATE_BASE + SIDE_GATE)) AND NOT l.granted") == 1 ]]; }
side_gate_open() { touch "$(dir "$1")/gate.open"; wait "$SIDE_GATE_PID"; }

# A session of a node that stays in a REPEATABLE READ transaction whose snapshot was taken at once, to read later under that snapshot
#   side_rr_open NODE   side_rr_run SQL...   (the output of the statements)   side_rr_close
side_rr_open() {
	local d
	d=$(dir "$1")
	rm -f "$d/rr.in" "$d/rr.out"
	mkfifo "$d/rr.in"
	psql -X -At -q -h "$d/sock" -p "$(port "$1")" -U postgres -d postgres <"$d/rr.in" >"$d/rr.out" 2>&1 &
	SIDE_RR_PID=$!
	exec {SIDE_RR_FD}>"$d/rr.in"
	SIDE_RR_OUT=$d/rr.out
	side_rr_run "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT txid_current_snapshot()" >/dev/null
}
side_rr_run() {
	local from n s
	from=$(wc -l <"$SIDE_RR_OUT")
	n=$(($(grep -c '^sync' "$SIDE_RR_OUT") + 1))
	for s in "$@"; do echo "$s;" >&"$SIDE_RR_FD"; done
	echo "\\echo sync$n" >&"$SIDE_RR_FD"
	wait_for "grep -q '^sync$n\$' '$SIDE_RR_OUT'" 120 || bail "the REPEATABLE READ session did not answer"
	tail -n +$((from + 1)) "$SIDE_RR_OUT" | grep -v "^sync$n\$"
}
side_rr_close() { exec {SIDE_RR_FD}>&-; wait "$SIDE_RR_PID"; }
