#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The background workers of the maintenance, stopped in the middle of a step: a maintenance worker of a sift_col index held when it enters its promote phase (sift.test_progress_gate, a gate of the
# worker only, no sleep) and the worker that promotes a sealed buffer of a BM25 index held in its step (sift.test_progress_gate), each ended by a terminate of the worker and by an immediate
# shutdown of the server. Afterwards nobody holds a lock, no progress slot is left, the index verifies, the answers through it are the table's, and the maintenance resumes: the next request
# starts a worker that finishes what is due (no write buffer rows left in the sift_col index, no sealed buffer left of the BM25 index).
. "$(dirname "$0")/lib.bash"

node_init wk 1 "autovacuum = off
sift.test_col_net = off
sift.maintenance_naptime = 200
max_worker_processes = 8"
node_start wk || bail "start failed"
D=$(dir wk)
SIFT_TMP=$D leak_expect 20 'terminated by signal 9|terminating connection|terminating background worker'
sql wk "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

setup() { # KIND: a fresh table with an index of the kind
	sql wk "DROP TABLE IF EXISTS facts, docs CASCADE" >/dev/null
	case $1 in
		col) sql wk "CREATE TABLE facts (id int, key int, amount numeric(10,2), pad text)" \
			"INSERT INTO facts SELECT g, g % 1000, g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
			"CREATE INDEX facts_idx ON facts USING sift_col (id, key, amount)" "VACUUM facts" >/dev/null ;;
		bm25) sql wk "CREATE TABLE docs (id int, body text)" "CREATE INDEX docs_idx ON docs USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null ;;
	esac
}
trigger() { # KIND: makes the worker start
	case $1 in
		col) sql wk "UPDATE facts SET amount = amount + 1 WHERE id % 100 = 1" "DELETE FROM facts WHERE id % 100 = 51" \
			"INSERT INTO facts SELECT 100000 + g, g % 1000, g % 997, repeat('x', 150) FROM generate_series(1, 600) g" "VACUUM facts" >/dev/null ;;
		bm25) sql wk "INSERT INTO docs SELECT i, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 6000) i" >/dev/null ;;
	esac
}
configure() { # KIND on|off
	if [[ $2 == on ]]; then
		case $1 in
			col) sql wk "ALTER DATABASE postgres SET sift.async_maintenance = on" "ALTER DATABASE postgres SET sift.test_progress_gate = 1" >/dev/null ;;
			bm25) sql wk "ALTER DATABASE postgres SET sift.test_step_gate = on" >/dev/null ;;
		esac
	else
		sql wk "ALTER DATABASE postgres RESET sift.test_progress_gate" "ALTER DATABASE postgres RESET sift.test_step_gate" >/dev/null
	fi
}
KEY_col=12 KEY_bm25=17
waiting() { [[ $(sql wk "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND objid = $1 AND NOT granted") -ge 1 ]]; }
worker_pid() { sql wk "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND objid = $1 AND NOT granted LIMIT 1"; }
answers() { # KIND: the answers through the index and without it, equal
	local q off on
	case $1 in
		col) q="SELECT count(*), sum(amount), count(DISTINCT key) FROM facts"; off=("SET sift.enable_agg = off" "SET sift.enable_scan = off"); on=("SET sift.enable_agg = on" "SET seq_page_cost = 10000") ;;
		bm25) q="SELECT count(*), sum(id) FROM docs WHERE body @@> 'alpha3 beta4'"; off=("SET sift.enable_scan = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off"); on=("SET seq_page_cost = 10000" "SET enable_seqscan = off") ;;
	esac
	[[ $(sql wk "${off[@]}" "$q") == "$(sql wk "${on[@]}" "$q")" ]]
}
verified() { # KIND
	case $1 in
		col) sql wk "SELECT sift.col_verify('facts_idx', true)" "SELECT sift._test_check('facts_idx')" >/dev/null ;;
		bm25) sql wk "SELECT sift.verify('docs_idx', true)" "SELECT sift._test_check('docs_idx')" >/dev/null ;;
	esac
}
settled() { # KIND: the maintenance has nothing left
	case $1 in
		col) [[ $(sql wk "SELECT coalesce(sum(rows), 0) FROM sift.col_segments('facts_idx') WHERE kind <> 'immutable'") == 0 ]] ;;
		bm25) [[ $(sql wk "SELECT count(*) FROM sift.segments('docs_idx') WHERE kind LIKE 'wbuf%' OR kind LIKE 'sealed%'") == 0 ]] ;;
	esac
}

for kind in ${SIFT_WORKER_KINDS:-col bm25}; do
	for fault in terminate immediate; do
		id="$kind worker, $fault"
		key=KEY_$kind
		wait_for '[[ $(sql wk "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE '"'sift %'"'") == 0 ]]' 60 || bail "$id: a worker of the step before is still alive"
		setup "$kind"
		hold=
		sql wk "SELECT pg_advisory_lock(20983, ${!key}), pg_sleep(600)" >/dev/null 2>&1 &
		hold=$!
		wait_for "[[ \$(sql wk \"SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND objid = ${!key} AND granted\") -ge 1 ]]" 30 || bail "$id: the gate was not taken"
		configure "$kind" on
		trigger "$kind"
		check "$id: the worker waits at its gate" wait_for "waiting ${!key}" 60
		pid=$(worker_pid "${!key}")
		if [[ $fault == terminate ]]; then
			sql wk "SELECT pg_terminate_backend($pid)" >/dev/null
			wait_for "[[ -z \$(sql wk \"SELECT 1 FROM pg_stat_activity WHERE pid = $pid\") ]]" 30
			sql wk "SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND objid = ${!key} AND granted" >/dev/null
			wait "$hold" 2>/dev/null
		else
			pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1
			wait "$hold" 2>/dev/null
			node_start wk || bail "$id: restart failed"
		fi
		configure "$kind" off
		check "$id: nobody holds a lock" test "$(sql wk "SELECT count(*) FROM pg_locks WHERE pid IS DISTINCT FROM pg_backend_pid() AND locktype <> 'virtualxid'")" == 0
		check "$id: no progress slot is left" test "$(sql wk "SELECT count(*) FROM sift._progress()")" == 0
		check "$id: the index verifies" verified "$kind"
		check "$id: the answers through the index are the table's" answers "$kind"
		trigger "$kind"
		check "$id: the maintenance resumes and finishes" wait_for "settled $kind" 120
		check "$id: the index verifies after the maintenance" verified "$kind"
		check "$id: and answers as the table does" answers "$kind"
	done
done
done_testing
finish
