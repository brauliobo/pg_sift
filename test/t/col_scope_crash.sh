#!/usr/bin/env bash
# The crash matrix of the operations that publish a new directory of an index with a scope (test/t/col_crash_matrix.sh is the model): the build, a REINDEX, the promotion of a write buffer, a merge and a VACUUM
# of the facts of a sift_col index whose rows are also cut by a scope key. Every publication (store_update) is a named point of the testing build: sift.test_crash_dir = n kills the process with SIGKILL before
# the nth directory update (n > 0) or after it (n < 0), so that a scope segment is written and the directory is still the old one, or the directory is the new one and what follows did not run. The points are the
# first and the last of the updates of each operation (more in the full run). After the restart (wal_consistency_checking = generic) the index verifies (heapallindexed), the store checks pass, the answers
# through the scope path (the marker sift.in_scope, one key, a set of keys counted once, per pair) are the plain join's whatever the verdict of the reader (a read through the segments or its documented
# fallback), and the operation, run again, completes and leaves the same, for the scope entries too: after a build, a REINDEX, a promotion and a merge the scope entries are the join of the
# facts and the pairs (not after the VACUUM scenario, whose updates leave the new versions in the write buffer, nor the pairs scenario, which edits the pairs: the entries are those of the pairs as built). SIFT_CRASHMX_ITERS caps the number of crashes (default 40, 14 quick),
# SIFT_CRASHMX_ONLY a regex of scenario names.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_CRASHMX_ITERS 40 14)
FULL=$(knob SIFT_CRASHMX_FULL 1 "")
node_init scc 48 "wal_consistency_checking = 'generic'
autovacuum = off
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
shared_buffers = 64MB
include_if_exists = 'crash.conf'"
node_start scc || bail "start failed"
D=$(dir scc)
SIFT_TMP=$D leak_expect 200 'terminated by signal 9'
sql scc "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

# 60 keys, 300 members in 0..6 of them (members 100..109 in none), 6,000 facts (some without a member, some of an unknown one)
BASE="CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
	INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 60 FROM generate_series(1, 300) m, generate_series(0, 5) j WHERE j < m % 7 AND m NOT BETWEEN 100 AND 109;
	CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
	CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
	INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
		timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 6000) i"
more() { echo "INSERT INTO events SELECT i, 1 + i % 320, timestamptz '2026-06-01' + i * interval '1 minute', i % 10, 'y' FROM generate_series($1, $2) i"; }
IDX="CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (write_buffer_limit = 4096, scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')"
IDXR="$IDX; REINDEX INDEX events_col"   # the first scope index of a relation takes its stamps by a REINDEX

# scenario ~ setup (after the base tables) ~ the operation ~ whether the scope entries are the join of the facts and the pairs after the operation
SCENARIOS=(
	"scope_build~~$IDX~1"
	"scope_reindex~$IDX; $(more 6001 6600)~REINDEX INDEX events_col~1"
	"scope_promote~$IDXR; $(more 6001 8000)~SELECT sift.col_promote('events_col')~1"
	"scope_merge~$IDXR; $(more 6001 7000); SELECT sift.col_promote('events_col'); $(more 7001 8000); SELECT sift.col_promote('events_col')~SELECT sift.col_merge('events_col', true)~1"
	"scope_vacuum~$IDXR; DELETE FROM events WHERE id % 3 = 0; UPDATE events SET v = v + 1 WHERE id % 7 = 0~VACUUM events~0"
	"scope_pairs~$IDXR; UPDATE tenant_scope SET tenant_id = 1 + tenant_id % 60 WHERE dim_a_id % 5 = 0; $(more 6001 6500)~SELECT sift.col_promote('events_col')~0"
)

# the answers in a mode: one key, a set counted once, per pair (the scope forms and the joins as written), a plain statement over the facts
Q_ONE="SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, 7)"
Q_SET="SELECT count(*), sum(v), count(DISTINCT dim_a_id) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, ARRAY[3, 8, 21, 59]::int8[])"
Q_PAIR="SELECT k, (SELECT count(*) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, k)) FROM generate_series(1, 61) k ORDER BY k"
P_ONE="SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 7)"
P_SET="SELECT count(*), sum(v), count(DISTINCT dim_a_id) FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id IN (3, 8, 21, 59))"
P_PAIR="SELECT k, (SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k) FROM generate_series(1, 61) k ORDER BY k"
PLAIN=("SET sift.enable_agg = off" "SET sift.enable_scan = off" "SET sift.enable_count = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off")
IDXMODE=("SET sift.enable_agg = on" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0")
ENTRIES="SELECT (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col'))
	= (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id)"
VERIFY="DO \$\$ DECLARE r record; BEGIN
	FOR r IN SELECT c.oid::regclass AS ix FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' LOOP
		PERFORM sift.col_verify(r.ix, true);
		PERFORM sift._test_check(r.ix);
	END LOOP; END \$\$"
has_index() { [[ $(sql scc "SELECT to_regclass('events_col') IS NOT NULL") == t ]]; }
healthy() { # the indexes verify, and the plain answers are REF's; the scope forms (the reader's own verdict) are the plain join's, and so are the planned plain forms
	local plain
	sql scc "$VERIFY" >&2 || return 1
	plain=$(sql scc "${PLAIN[@]}" "$P_ONE" "$P_SET" "$P_PAIR" 2>&1)
	[[ $plain == "$REF" ]] || { diag "the plain answers changed"; return 1; }
	has_index || return 0
	[[ $(sql scc "${IDXMODE[@]}" "$P_ONE" "$P_SET" "$P_PAIR" 2>&1) == "$plain" ]] || { diag "the plain forms through the index differ from the plain plan"; return 1; }
	[[ $(sql scc "${IDXMODE[@]}" "$Q_ONE" "$Q_SET" "$Q_PAIR" 2>&1) == "$plain" ]] || { diag "the scope forms differ from the join"; return 1; }
}
settled() { # after the operation ran again: the entries are the join where they must be
	healthy || return 1
	has_index || return 1
	[[ $ENTRIES_JOIN == 0 || $(sql scc "$ENTRIES") == t ]] || { diag "the scope entries are not the join of the facts and the pairs"; return 1; }
}
fresh() { # SETUP
	sql scc "DROP TABLE IF EXISTS events, tenant_scope CASCADE" "$BASE" >/dev/null || return 1
	[[ -z $1 ]] || sql scc "$1" >/dev/null || return 1
	REF=$(sql scc "${PLAIN[@]}" "$P_ONE" "$P_SET" "$P_PAIR" 2>&1)
}
await_exit() { # PID
	for _ in $(seq 600); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
	return 1
}

total=0
for row in "${SCENARIOS[@]}"; do
	IFS='~' read -r name setup op build_joins <<<"$row"
	[[ -z ${SIFT_CRASHMX_ONLY:-} || $name =~ $SIFT_CRASHMX_ONLY ]] || continue
	ENTRIES_JOIN=$build_joins
	fresh "$setup" || bail "$name: setup"
	healthy || bail "$name: the starting state is not healthy"
	# the directory updates of the operation, counted in a session of its own
	k=$(sql scc "$op" "SELECT sift._test_dir_updates()" | tail -1)
	(( k > 0 )) || bail "$name: the operation published nothing"
	points=(1 "-$k")
	[[ -z $FULL ]] || points+=(-1 "$k" $(( (k + 1) / 2 )))
	declare -A seen=()
	for n in "${points[@]}"; do
		[[ -z ${seen[$n]:-} && $n != 0 ]] || continue
		seen[$n]=1
		(( ++total <= ITERS )) || break 2
		fresh "$setup" || bail "$name: setup"
		pm=$(head -1 "$D/data/postmaster.pid")
		sql scc "SET sift.test_crash_dir = $n" "$op" >/dev/null 2>&1
		if ! await_exit "$pm"; then
			not_ok "$name, update $n of $k: the process was not killed"
			break 2
		fi
		if ! node_start scc; then
			not_ok "$name, update $n of $k: restart"
			diag "$(tail -20 "$D/postgres.log")"
			break 2
		fi
		if healthy; then
			sql scc "$op" >/dev/null && settled && ok "$name, kill $([[ $n -gt 0 ]] && echo before || echo after) update ${n#-} of $k: recovery, answers, the operation again" ||
				{ not_ok "$name, kill at $n: after the operation run again"; diag "$(tail -5 "$D/postgres.log")"; }
		else
			not_ok "$name, kill at $n of $k: after recovery"
		fi
	done
	unset seen
done
echo "1..$NTEST"
finish
