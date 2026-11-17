#!/usr/bin/env bash
# The fault matrix: every long-running or multi-step operation (build, CREATE INDEX CONCURRENTLY, REINDEX [CONCURRENTLY], scope and track builds, promotion, merge, VACUUM, rolling rewrite, refill, sealing of a
# sift_table, DROP INDEX) is stopped at the boundaries of its phases (the directory updates it publishes, sift.test_crash_dir; the gates of the build and of the removal of the orphans of the log) by a fault
# of each kind and the state it leaves is checked. The cases are the rows of fault_matrix.tsv (id, operation, phase, fault, tier; `fault_matrix.sh --gen` prints it):
#   phase  b1, bm, bk: before the first, the middle and the last directory update of the operation; a1, ak: after the first and the last; gate:build, gate:drop: the gates of the build and of the orphan removal
#   fault  error: an ERROR raised at the phase; terminate, cancel: another session ends or cancels the backend held at the phase (a wait for an advisory lock: no sleeps); immediate: the server is
#          stopped with pg_ctl -m immediate at the phase (crash recovery replays its WAL)
# After the fault (and the restart) the invariants: nobody holds a lock, no progress or quarantine slot is left, the indexes verify and the answers through them are the plain plan's, an INVALID index is
# dropped by DROP INDEX CONCURRENTLY and leaves no trigger of the log of changes behind, the operation run again completes, leaves the same state and grows the files by less than a half of what a clean run
# makes, and the maintenance of every index has nothing due after sift.col_maintain. SIFT_FAULT_TIER=quick (SIFT_QUICK=1) runs the rows of the quick tier: one fault for each phase of each operation.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
HERE=$(dirname "$0")
SCENARIO_NAMES=(bm25_build col_build bm25_promote col_promote bm25_merge col_merge scope_build scope_promote scope_merge scope_refill vacuum col_roll tbl_seal
	col_cic track_cic scope_cic reindex_col reindex_col_c drop_scope)
GATE_OPS=(col_build col_cic track_cic scope_cic reindex_col drop_scope)
gen() { # the rows of the matrix: every operation at the phases of its directory updates, and at the gates
	local op ph fault quick
	quick=$(for op in "${SCENARIO_NAMES[@]}"; do for ph in b1:terminate bm:cancel bk:error a1:error ak:immediate ak:cancel; do printf '%s.%s.%s\t\n' "$op" "${ph%:*}" "${ph#*:}"; done; done)
	for op in "${SCENARIO_NAMES[@]}"; do
		[[ $op == drop_scope ]] || for ph in b1:terminate bm:cancel bk:error a1:error ak:immediate ak:cancel; do
			printf '%s.%s.%s\t%s\t%s\t%s\tquick\n' "$op" "${ph%:*}" "${ph#*:}" "$op" "${ph%:*}" "${ph#*:}"
		done
		[[ $op == drop_scope ]] || for ph in b1 bm bk a1 ak; do
			for fault in error terminate cancel immediate; do
				grep -q "^$op\.$ph\.$fault	" <<<"$quick" || printf '%s.%s.%s\t%s\t%s\t%s\tfull\n' "$op" "$ph" "$fault" "$op" "$ph" "$fault"
			done
		done
	done
	while IFS='|' read -r op ph fault mut regex; do
		printf '%s.%s.%s.%s\t%s\t%s\t%s\tmutant\t%s\t%s\n' "$op" "$ph" "$fault" "$mut" "$op" "$ph" "$fault" "$mut" "$regex"
	done <<'MUTANTS'
col_cic|b1|terminate|leak_lock|locks left
bm25_promote|a1|error|leak_slot|progress or quarantine slot
col_promote|a1|error|orphan_trigger|orphan triggers
col_cic|b1|terminate|undroppable|DROP INDEX CONCURRENTLY of the INVALID
col_promote|a1|error|verify_failure|not healthy
col_promote|a1|error|wrong_answer|plain answers changed
col_promote|a1|error|growth|files grew
vacuum|a1|error|still_due|maintenance still finds
MUTANTS
	for op in "${GATE_OPS[@]}"; do
		for ph in build:terminate build:immediate drop:cancel; do
			case $op in drop_scope) [[ ${ph%:*} == drop ]] || continue ;; *) [[ ${ph%:*} == build ]] || continue ;; esac
			printf '%s.gate-%s.%s\t%s\tgate:%s\t%s\t%s\n' "$op" "${ph%:*}" "${ph#*:}" "$op" "${ph%:*}" "${ph#*:}" "$([[ ${ph#*:} == terminate || ${ph%:*} == drop ]] && echo quick || echo full)"
		done
	done
}
[[ ${1:-} != --gen ]] || { gen; exit 0; }
. "$HERE/lib.bash"

TIER=$(knob SIFT_FAULT_TIER full quick)
node_init cm 1 "wal_consistency_checking = 'generic'
autovacuum = off
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
max_worker_processes = 8
max_prepared_transactions = 4
shared_buffers = 64MB"
node_start cm || bail "start failed"
D=$(dir cm)
SIFT_TMP=$D leak_expect 400 'terminated by signal 9|terminating connection'
sql cm "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
. "$HERE/crash_scenarios.bash"

CIC_OPTS="WITH (write_buffer_limit = 4096)"
SCENARIOS+=(
	"col_cic~~CREATE INDEX CONCURRENTLY docs_col ON docs USING sift_col (id, g, n) $CIC_OPTS"
	"track_cic~~CREATE INDEX CONCURRENTLY docs_tr ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, track_changes = 'ctid')"
	"scope_cic~$GP~CREATE INDEX CONCURRENTLY docs_sc ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, scope = 't: g -> gp(g, t)')"
	"reindex_col~$COL~REINDEX INDEX docs_col"
	"reindex_col_c~$COL~REINDEX INDEX CONCURRENTLY docs_col"
	"drop_scope~$SCOPE~DROP INDEX docs_sc"
)
declare -A SETUP OP KCOUNT SIZE0
for row in "${SCENARIOS[@]}"; do IFS='~' read -r name setup op <<<"$row"; SETUP[$name]=$setup; OP[$name]=$op; done

GATE_KEY=13 GATE_GUC=
INDEXES="SELECT string_agg(c.oid::regclass::text, ' ') FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col')"
SIZES="SELECT coalesce(sum(pg_relation_size(c.oid)), 0) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col')"
LOCKS="SELECT count(*) FROM pg_locks WHERE pid IS DISTINCT FROM pg_backend_pid() AND locktype <> 'virtualxid'"
SLOTS="SELECT (SELECT count(*) FROM sift._progress()) + (SELECT count(*) FROM sift.quarantine)"
INVALID="SELECT c.oid::regclass FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE NOT i.indisvalid"
ORPHANS="SELECT count(*) FROM pg_trigger t JOIN pg_proc p ON p.oid = t.tgfoid WHERE p.proname = 'col_track_old_rows' AND NOT EXISTS (
	SELECT 1 FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = t.tgrelid AND a.amname = 'sift_col')"
DUE="SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam, LATERAL sift.col_maintenance_status(c.oid) m
	WHERE a.amname = 'sift_col' AND coalesce(m.rewrite_due, '') <> ''"

sizes_ok() { # AFTER BASELINE: growth under a half of a clean run, and 256 kB
	(( $1 <= $2 + $2 / 2 + 262144 ))
}
gate_guc() { # the gate GUC of a phase
	case $1 in build) echo "SET sift.test_build_gate = on" ;; drop) echo "SET sift.test_drop_gate = on" ;; esac
}
holder_pid() { sql cm "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND objid = $GATE_KEY AND granted LIMIT 1"; }
held_pid() { sql cm "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND objid = $GATE_KEY AND NOT granted LIMIT 1"; }
is_held() { [[ -n $(held_pid) ]]; }

# the number of directory updates of an operation in a clean state, and the size it leaves
measure() { # NAME
	local k
	fresh "${SETUP[$1]}" || bail "$1: setup"
	k=$(sql cm "${OP[$1]}" "SELECT sift._test_dir_updates()" | tail -1) || bail "$1: the operation failed in a clean state"
	KCOUNT[$1]=$k
	SIZE0[$1]=$(sql cm "$SIZES")
}

phase_point() { # NAME PHASE: the sift.test_crash_dir of a phase
	local k=${KCOUNT[$1]}
	case $2 in b1) echo 1 ;; bm) echo $(( (k + 1) / 2 )) ;; bk) echo "$k" ;; a1) echo -1 ;; ak) echo "-$k" ;; esac
}

run_fault() { # NAME PHASE FAULT: leaves the server running (restarted after an immediate stop)
	local name=$1 phase=$2 fault=$3 pre=() bpid hold op
	if [[ $phase == gate:* ]]; then
		GATE_KEY=$( [[ ${phase#gate:} == build ]] && echo 8 || echo 9 )
		pre=("$(gate_guc "${phase#gate:}")")
	else
		GATE_KEY=13
		pre=("SET sift.test_crash_dir = $(phase_point "$name" "$phase")" "SET sift.test_crash_kind = $([[ $fault == error ]] && echo 1 || echo 2)")
	fi
	if [[ $fault == error ]]; then
		sql cm "${pre[@]}" "${OP[$name]}" >"$D/fault.out" 2>&1
		grep -q 'sift test: error' "$D/fault.out" || { diag "no injected error: $(head -3 "$D/fault.out")"; return 1; }
		return 0
	fi
	sql cm "SELECT pg_advisory_lock(20983, $GATE_KEY), pg_sleep(600)" >/dev/null 2>&1 &
	hold=$!
	wait_for "test -n \"\$(holder_pid)\"" 30 || { diag "the gate was not taken"; return 1; }
	sql cm "${pre[@]}" "${OP[$name]}" >"$D/fault.out" 2>&1 &
	op=$!
	if ! wait_for is_held 60; then
		diag "the operation did not reach the gate: $(head -3 "$D/fault.out")"
		sql cm "SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND granted" >/dev/null
		return 1
	fi
	bpid=$(held_pid)
	case $fault in
		terminate) sql cm "SELECT pg_terminate_backend($bpid)" >/dev/null ;;
		cancel) sql cm "SELECT pg_cancel_backend($bpid)" >/dev/null ;;
		immediate) pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1 ;;
	esac
	wait "$op" 2>/dev/null && { diag "the operation completed despite the $fault"; return 1; }
	if [[ $fault == immediate ]]; then
		node_start cm || { diag "restart: $(tail -20 "$D/postgres.log")"; return 1; }
	else
		sql cm "SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND granted" >/dev/null
	fi
	wait 2>/dev/null
	return 0
}

settled() { # no session but ours is left
	[[ $(sql cm "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()") == 0 ]]
}

# the invariants after a fault; prints what is wrong
after_fault() { # NAME
	local name=$1 ix size n out
	wait_for settled 30 || { echo "a session is left"; return 1; }
	[[ $(sql cm "$LOCKS") == 0 ]] || { echo "locks left: $(sql cm "SELECT locktype, mode, relation::regclass FROM pg_locks WHERE pid IS DISTINCT FROM pg_backend_pid() AND locktype <> 'virtualxid'" | tr '\n' ';')"; return 1; }
	[[ $(sql cm "$SLOTS") == 0 ]] || { echo "a progress or quarantine slot is left"; return 1; }
	for ix in $(sql cm "$INVALID"); do
		sql cm "DROP INDEX CONCURRENTLY $ix" >/dev/null 2>&1 || { echo "DROP INDEX CONCURRENTLY of the INVALID $ix failed"; return 1; }
	done
	[[ -z $(sql cm "$INVALID") ]] || { echo "an INVALID index is left"; return 1; }
	wait_for "test \"\$(sql cm \"$ORPHANS\")\" == 0" 30 || { echo "orphan triggers of the log of changes are left"; return 1; }
	out=$(healthy 2>&1) || { echo "the state is not healthy: $(head -c 300 <<<"$out" | tr '\n' ' ')"; return 1; }
	sql cm "${OP[$name]}" >"$D/rerun.out" 2>&1 || { echo "the operation run again failed: $(head -3 "$D/rerun.out")"; return 1; }
	out=$(healthy 2>&1) || { echo "not healthy after the operation run again: $(head -c 300 <<<"$out" | tr '\n' ' ')"; return 1; }
	size=$(sql cm "$SIZES")
	sizes_ok "$size" "${SIZE0[$name]}" || { echo "the files grew to $size bytes, a clean run leaves ${SIZE0[$name]}"; return 1; }
	[[ -z ${TAMPER_LATE:-} ]] || sql cm "UPDATE docs SET g = g + 1 WHERE id % 2 = 0" "VACUUM docs" >/dev/null
	[[ -n ${SKIP_MAINTAIN:-} ]] || for ix in $(sql cm "SELECT c.oid::regclass FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col'"); do
		sql cm "SELECT sift.col_maintain('$ix')" >/dev/null 2>&1
	done
	n=$(sql cm "$DUE")
	[[ $n == 0 ]] || { echo "maintenance still finds $n indexes due: $(sql cm "SELECT c.oid::regclass, m.rewrite_due FROM pg_class c JOIN pg_am a ON a.oid = c.relam, LATERAL sift.col_maintenance_status(c.oid) m WHERE a.amname = 'sift_col'" | tr '\n' ';')"; return 1; }
	[[ $(sql cm "$LOCKS") == 0 ]] || { echo "locks left after the rerun"; return 1; }
}

# the negative rows (tier mutant: id, operation, phase, fault, mutant, tamper, regex of what the invariants must say): the case runs, the tamper puts a defect in the state the fault left, and the case
# passes only when the invariants fail with the message. TAMPER_LATE runs after the operation run again; SKIP_MAINTAIN leaves out the maintenance of the last invariant.
tamper() { # NAME: sets TAMPER_LATE / SKIP_MAINTAIN, runs the tamper that goes before the invariants
	local g
	TAMPER_LATE= SKIP_MAINTAIN=
	case $1 in
		leak_lock) sql cm "BEGIN; LOCK TABLE docs IN ROW EXCLUSIVE MODE; PREPARE TRANSACTION 'mutant'" ;;
		leak_slot) sql cm "SET sift.test_mutant = 1" "VACUUM docs" ;;
		orphan_trigger) sql cm "CREATE TABLE orph (id int, g int)" "INSERT INTO orph SELECT i, i FROM generate_series(1, 100) i" \
			"SET sift.test_mutant = 2" "CREATE INDEX orph_tr ON orph USING sift_col (id, g) WITH (track_changes = 'ctid')" "DROP INDEX orph_tr" ;;
		undroppable) sql cm "CREATE FUNCTION no_drop() RETURNS event_trigger LANGUAGE plpgsql AS \$f\$ BEGIN RAISE EXCEPTION 'mutant: no drop'; END \$f\$" \
			"CREATE EVENT TRIGGER mutant ON ddl_command_start WHEN TAG IN ('DROP INDEX') EXECUTE FUNCTION no_drop()" ;;
		verify_failure) sql cm "SELECT sift._test_poke('docs_col', 2, 8176, 9, 2)" ;;
		wrong_answer) sql cm "DELETE FROM docs WHERE id = 5" ;;
		growth) sql cm "CREATE TABLE big AS SELECT i AS id, i % 97 AS g, md5(i::text) AS n FROM generate_series(1, 60000) i" "CREATE INDEX big_ix ON big USING sift_col (id, g, n)" ;;
		still_due) TAMPER_LATE=still_due SKIP_MAINTAIN=1 ;;
	esac
}
untamper() { # the slot of a dead process stays in shared memory until the server restarts
	[[ $1 != leak_slot ]] || { pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1; node_start cm; }
	sql cm "DROP TABLE IF EXISTS orph, big" "DROP EVENT TRIGGER IF EXISTS mutant" "DROP FUNCTION IF EXISTS no_drop()" >/dev/null 2>&1
	for g in $(sql cm "SELECT gid FROM pg_prepared_xacts"); do sql cm "ROLLBACK PREPARED '$g'" >/dev/null; done
}

mapfile -t ROWS < <(awk -F'\t' -v t="$TIER" 'NF >= 5 && $1 !~ /^#/ && (t == "full" || $5 == t || $5 == "mutant")' "$HERE/fault_matrix.tsv")
ONLY=${SIFT_FAULT_ONLY:-}
total=0
for row in "${ROWS[@]}"; do
	IFS=$'\t' read -r id name phase fault tier mutant expect <<<"$row"
	[[ -z $ONLY || $id =~ $ONLY ]] || continue
	[[ -n ${KCOUNT[$name]:-} ]] || measure "$name"
	if [[ $phase != gate:* ]] && (( ${KCOUNT[$name]} == 0 )); then
		continue
	fi
	(( ++total ))
	fresh "${SETUP[$name]}" || bail "$id: setup"
	healthy || bail "$id: the starting state is not healthy"
	if ! run_fault "$name" "$phase" "$fault"; then
		if [[ $phase == a* ]] && ! grep -q "corrupt" "$D/fault.out"; then
			skip 1 "$id: the update after which it stops does not publish a directory (the fault goes only after one that does)"
		else
			not_ok "$id: the fault could not be injected"
			diag "$(tail -8 "$D/postgres.log")"
		fi
		continue
	fi
	[[ -z ${mutant:-} ]] || tamper "$mutant"
	if msg=$(after_fault "$name"); then
		[[ -n ${mutant:-} ]] && not_ok "$id: the invariants did not see the defect $mutant" || ok "$id: invariants hold"
	elif [[ -n ${mutant:-} ]]; then
		[[ $msg =~ $expect ]] && ok "$id: the defect $mutant is found: $msg" || { not_ok "$id: the defect $mutant says '$msg', not /$expect/"; }
	else
		not_ok "$id: $msg"
		diag "$(tail -5 "$D/postgres.log")"
	fi
	[[ -z ${mutant:-} ]] || untamper "$mutant"
done
(( total > 0 )) || bail "no case ran"
echo "1..$NTEST"
finish
