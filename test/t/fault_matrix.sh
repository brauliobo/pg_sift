#!/usr/bin/env bash
# The fault matrix: every long-running or multi-step operation (build, CREATE INDEX CONCURRENTLY, REINDEX [CONCURRENTLY], scope and track builds, promotion, merge, VACUUM, rolling rewrite, refill, sealing of a
# sift_table, DROP INDEX) is stopped at the boundaries of its phases (the directory updates it publishes, sift.test_crash_dir; the gates of the build and of the removal of the orphans of the log) by a fault
# of each kind and the state it leaves is checked. The cases are the rows of fault_matrix.tsv (id, operation, phase, fault, tier; `fault_matrix.sh --gen` prints it):
#   phase  b1, bm, bk: before the first, the middle and the last directory update of the operation; a1, ak: after the first and the last; gate:build, gate:drop: the gates of the build and of the orphan removal
#   fault  error: an ERROR raised at the phase; terminate, cancel: another session ends or cancels the backend held at the phase (a wait for an advisory lock: no sleeps); immediate: the server is
#          stopped with pg_ctl -m immediate at the phase (crash recovery replays its WAL)
# After the fault (and the restart) the invariants: nobody holds a lock, no progress or quarantine slot is left, the indexes verify and the answers through them are the plain plan's, an INVALID index is
# dropped by DROP INDEX CONCURRENTLY and leaves no trigger of the log of changes behind, the operation run again completes, leaves the same state and grows the files by less than a half of what a clean run
# makes, and the maintenance of every index has nothing due after sift.col_maintain. SIFT_FAULT_TIER=quick (SIFT_QUICK=1) runs the rows of the quick tier: one fault for each phase of each operation. SIFT_FAULT_SLICE=K/N runs the Kth of N slices of the operations, balanced by their rows.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
HERE=$(dirname "$0")
SCENARIO_NAMES=(bm25_build col_build bm25_promote col_promote bm25_merge col_merge scope_build scope_promote scope_merge scope_refill vacuum col_roll tbl_seal
	col_cic track_cic scope_cic scope_cic_nj reindex_col reindex_col_c reindex_scope_c tbl_tomb tbl_copy
	alter_refresh alter_reloption col_repair)
GATES=(col_build:build col_cic:build track_cic:build scope_cic:build scope_cic:tail scope_cic:fill scope_cic_nj:journal reindex_col:build drop_track:drop vacuum:bulkdelete
	vacuum:cleanup alter_reloption:alter col_repair:repair_before col_repair:repair_after tbl_tomb:tombwrite tbl_tomb:tombmerge tbl_seal:tombrewrite tbl_copy:load
	f2_col_build:f2manifest f2_reindex:f2manifest f2_vacuum:f2manifest f2_compact:f2compact f2_tier:f2tier f2_scope_build:f2scope f2_scope_refill:f2scope f2_col_delta:f2coldelta)
gen() { # the rows of the matrix: every operation at the phases of its directory updates, and at the gates
	local op ph fault quick
	quick=$(for op in "${SCENARIO_NAMES[@]}"; do for ph in b1:terminate bm:cancel bk:error a1:error ak:immediate ak:cancel; do printf '%s.%s.%s\t\n' "$op" "${ph%:*}" "${ph#*:}"; done; done)
	for op in "${SCENARIO_NAMES[@]}"; do
		[[ " alter_reloption col_repair tbl_tomb " != *" $op "* ]] || continue
		for ph in b1:terminate bm:cancel bk:error a1:error ak:immediate ak:cancel; do
			printf '%s.%s.%s\t%s\t%s\t%s\tquick\n' "$op" "${ph%:*}" "${ph#*:}" "$op" "${ph%:*}" "${ph#*:}"
		done
		for ph in b1 bm bk a1 ak; do
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
	for row in "${GATES[@]}"; do
		op=${row%:*} ph=${row#*:}
		for fault in terminate cancel immediate; do
			printf '%s.gate-%s.%s\t%s\tgate:%s\t%s\t%s\n' "$op" "$ph" "$fault" "$op" "$ph" "$fault" \
				"$([[ $fault == terminate || ($ph == drop && $fault == cancel) ]] && echo quick || echo full)"
		done
	done
}
[[ ${1:-} != --gen ]] || { gen; exit 0; }
. "$HERE/lib.bash"

TIER=$(knob SIFT_FAULT_TIER full quick)
ORPHAN_SECONDS=$(knob SIFT_FAULT_ORPHAN_SECONDS 30 10)
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

declare -A SETUP OP KCOUNT SIZE0 DUE0
for row in "${SCENARIOS[@]}"; do IFS='~' read -r name setup op <<<"$row"; SETUP[$name]=$setup; OP[$name]=$op; done

INDEXES="SELECT string_agg(c.oid::regclass::text, ' ') FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col')"
SIZES="SELECT coalesce(sum(pg_relation_size(c.oid)), 0) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col')"
LOCKS="SELECT count(*) FROM pg_locks WHERE pid IS DISTINCT FROM pg_backend_pid() AND locktype <> 'virtualxid'"
SLOTS="SELECT (SELECT count(*) FROM sift._progress()) + (SELECT count(*) FROM sift.quarantine)"
INVALID="SELECT c.oid::regclass FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE NOT i.indisvalid"
ORPHANS="SELECT count(*) FROM pg_trigger t JOIN pg_proc p ON p.oid = t.tgfoid WHERE p.proname = 'col_track_old_rows' AND NOT EXISTS (
	SELECT 1 FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = t.tgrelid AND a.amname = 'sift_col')"
DUE="SELECT coalesce(string_agg(c.oid::regclass || ' ' || m.rewrite_due, ', ' ORDER BY c.oid::regclass::text), '') FROM pg_class c JOIN pg_am a ON a.oid = c.relam,
	LATERAL sift.col_maintenance_status(c.oid) m WHERE a.amname = 'sift_col' AND coalesce(m.rewrite_due, '') <> ''"
maintain_all() {
	local ix
	for ix in $(sql cm "SELECT c.oid::regclass FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col'"); do
		sql cm "SELECT sift.col_maintain('$ix')" >/dev/null 2>&1
	done
}

sizes_ok() { # AFTER BASELINE: growth under a half of a clean run, and 256 kB
	(( $1 <= $2 + $2 / 2 + 262144 ))
}
holder_pid() { sql cm "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND granted LIMIT 1"; }
held_pid() { sql cm "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND NOT granted LIMIT 1"; }
release_gate() {
	sql cm "SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND granted" >/dev/null
	exec 9>&-
}
is_held() { [[ -n $(held_pid) ]]; }
# the holder takes the lock and then idles on a pipe: a query that waits holds a snapshot, which CREATE INDEX CONCURRENTLY waits for before the updates of its last phases
hold_gate() {
	rm -f "$D/gate.fifo"
	mkfifo "$D/gate.fifo"
	psql -X -At -q -v ON_ERROR_STOP=1 -h "$D/sock" -p "$(port cm)" -U postgres -d postgres <"$D/gate.fifo" >/dev/null 2>&1 &
	exec 9>"$D/gate.fifo"
	echo "SELECT $GATE_LOCK;" >&9
}

# the number of directory updates of an operation in a clean state, and the size it leaves
measure() { # NAME
	local k
	fresh "${SETUP[$1]}" || bail "$1: setup"
	k=$(op_stmts "$1"; sql cm "${OPS[@]}" "SELECT sift._test_dir_updates()" | tail -1) || bail "$1: the operation failed in a clean state"
	KCOUNT[$1]=$k
	SIZE0[$1]=$(sql cm "$SIZES")
	maintain_all
	DUE0[$1]=$(sql cm "$DUE")
}

phase_point() { # NAME PHASE: the sift.test_crash_dir of a phase
	local k=${KCOUNT[$1]}
	case $2 in b1) echo 1 ;; bm) echo $(( (k + 1) / 2 )) ;; bk) echo "$k" ;; a1) echo -1 ;; ak) echo "-$k" ;; esac
}

run_fault() { # NAME PHASE FAULT: leaves the server running (restarted after an immediate stop)
	local name=$1 phase=$2 fault=$3 pre=() bpid op
	if [[ $phase == gate:* ]]; then
		gate_spec "${phase#gate:}"
		pre=("${GATE_SET[@]}")
	else
		gate_spec dir
		pre=("SET sift.test_crash_dir = $(phase_point "$name" "$phase")" "SET sift.test_crash_kind = $([[ $fault == error ]] && echo 1 || echo 2)")
	fi
	if [[ $fault == error ]]; then
		op_stmts "$name"; sql cm "${pre[@]}" "${OPS[@]}" >"$D/fault.out" 2>&1
		grep -q 'sift test: error' "$D/fault.out" || { diag "no injected error: $(head -3 "$D/fault.out")"; return 1; }
		return 0
	fi
	hold_gate
	wait_for "test -n \"\$(holder_pid)\"" 30 || { diag "the gate was not taken"; release_gate; wait 2>/dev/null; return 1; }
	op_stmts "$name"; sql cm "${pre[@]}" "${OPS[@]}" >"$D/fault.out" 2>&1 &
	op=$!
	wait_for "is_held || ! kill -0 $op 2>/dev/null" 60
	if ! is_held; then
		diag "the operation did not reach the gate: $(head -3 "$D/fault.out")"
		release_gate
		wait 2>/dev/null
		return 1
	fi
	bpid=$(held_pid)
	case $fault in
		terminate) sql cm "SELECT pg_terminate_backend($bpid)" >/dev/null ;;
		cancel) sql cm "SELECT pg_cancel_backend($bpid)" >/dev/null ;;
		immediate) pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1 ;;
	esac
	if [[ $fault != immediate ]] && ! wait_for "! kill -0 $op 2>/dev/null" 10; then
		release_gate
		wait 2>/dev/null
		HELD_INTERRUPTS=1
		return 1
	fi
	wait "$op" 2>/dev/null && { diag "the operation completed despite the $fault"; return 1; }
	if [[ $fault == immediate ]]; then
		exec 9>&-
		node_start cm || { diag "restart: $(tail -20 "$D/postgres.log")"; return 1; }
	else
		release_gate
	fi
	wait 2>/dev/null
	return 0
}

settled() { # no session but ours is left, and no background worker of sift (async maintenance hands its work to one) is at work
	[[ $(sql cm "SELECT count(*) FROM pg_stat_activity WHERE (backend_type = 'client backend' OR backend_type LIKE 'sift %') AND pid <> pg_backend_pid()") == 0 ]]
}

# the invariants after a fault; prints what is wrong
after_fault() { # NAME
	local name=$1 ix size n out
	wait_for settled 30 || { echo "a session is left"; return 1; }
	# first: a session that loads the library in a transaction removes the orphans of the log, as the sessions of the checks below do
	wait_for "test \"\$(sql cm \"$ORPHANS\")\" == 0" "${ORPHAN_WAIT:-$ORPHAN_SECONDS}" || { echo "orphan triggers of the log of changes are left"; return 1; }
	[[ $(sql cm "$LOCKS") == 0 ]] || { echo "locks left: $(sql cm "SELECT l.pid, a.backend_type, l.locktype, l.mode, l.classid, l.objid, l.objsubid, l.relation::regclass, l.granted FROM pg_locks l LEFT JOIN pg_stat_activity a USING (pid) WHERE l.pid IS DISTINCT FROM pg_backend_pid() AND l.locktype <> 'virtualxid'" | tr '\n' ';')"; return 1; }
	[[ $(sql cm "$SLOTS") == 0 ]] || { echo "a progress or quarantine slot is left"; return 1; }
	for _ in 1 2 3; do # an index another one depends on goes after it
		for ix in $(sql cm "$INVALID"); do
			out=$(sql cm "DROP INDEX CONCURRENTLY IF EXISTS $ix" 2>&1) || [[ $out == *"other objects depend"* ]] ||
				{ echo "DROP INDEX CONCURRENTLY of the INVALID $ix failed: $(head -c 300 <<<"$out" | tr '\n' ' ')"; return 1; }
		done
	done
	[[ -z $(sql cm "$INVALID") ]] || { echo "an INVALID index is left"; return 1; }
	[[ $DAMAGED == *" $name "* ]] || out=$(healthy 2>&1) || { echo "the state is not healthy: $(head -c 300 <<<"$out" | tr '\n' ' ')"; return 1; }
	op_stmts "$name"; sql cm "${OPS[@]}" >"$D/rerun.out" 2>&1 || { echo "the operation run again failed: $(head -3 "$D/rerun.out")"; return 1; }
	[[ $DATA_OPS != *" $name "* ]] || REF=$(answers PLAIN)
	out=$(healthy 2>&1) || { echo "not healthy after the operation run again: $(head -c 300 <<<"$out" | tr '\n' ' ')"; return 1; }
	size=$(sql cm "$SIZES")
	sizes_ok "$size" "${SIZE0[$name]}" || { echo "the files grew to $size bytes, a clean run leaves ${SIZE0[$name]}"; return 1; }
	[[ -z ${TAMPER_LATE:-} ]] || sql cm "UPDATE docs SET g = g + 1 WHERE id % 2 = 0" "VACUUM docs" >/dev/null
	[[ -n ${SKIP_MAINTAIN:-} ]] || maintain_all
	n=$(sql cm "$DUE")
	[[ $n == "${DUE0[$name]}" ]] || { echo "maintenance still finds work due ($n), a clean run leaves (${DUE0[$name]})"; return 1; }
	[[ $(sql cm "$LOCKS") == 0 ]] || { echo "locks left after the rerun"; return 1; }
}

# the negative rows (tier mutant: id, operation, phase, fault, mutant, tamper, regex of what the invariants must say): the case runs, the tamper puts a defect in the state the fault left, and the case
# passes only when the invariants fail with the message. TAMPER_LATE runs after the operation run again; SKIP_MAINTAIN leaves out the maintenance of the last invariant.
tamper() { # NAME: sets TAMPER_LATE / SKIP_MAINTAIN, runs the tamper that goes before the invariants
	local g
	TAMPER_LATE= SKIP_MAINTAIN= ORPHAN_WAIT=
	case $1 in
		leak_lock) sql cm "BEGIN; LOCK TABLE docs IN ROW EXCLUSIVE MODE; PREPARE TRANSACTION 'mutant'" ;;
		leak_slot) sql cm "SET sift.test_mutant = 1" "VACUUM docs" ;;
		orphan_trigger) ORPHAN_WAIT=1   # the hook of the mutant stops the removal: nothing is left to wait for
			sql cm "CREATE TABLE orph (id int, g int)" "INSERT INTO orph SELECT i, i FROM generate_series(1, 100) i" \
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

# the selftest of the negative rows: every hook a mutant needs is in the source, every mutant has a tamper, and a run of the whole tier has all of them (a missing hook or tamper fails the
# run here, and a mutant whose defect the invariants do not see fails its own row)
mutants=$(awk -F'\t' '$5 == "mutant" {print $6}' "$HERE/fault_matrix.tsv" | sort -u)
SLICE=${SIFT_FAULT_SLICE:-1/1}
[[ -n ${SIFT_FAULT_ONLY:-} || ${SLICE%/*} != 1 ]] || {
	check "the tier has the eight mutants" test "$(wc -w <<<"$mutants")" == 8
	for m in $mutants; do check "the mutant $m has its tamper" grep -q "^		$m) " "$HERE/fault_matrix.sh"; done
	for k in 1 2; do check "the library has the hook of the mutant kind $k" grep -rq "SIFT_TEST_MUTANT($k," "$HERE/../../src"; done
}

mapfile -t ROWS < <(awk -F'\t' -v t="$TIER" 'NF >= 5 && $1 !~ /^#/ && (t == "full" || $5 == t || $5 == "mutant")' "$HERE/fault_matrix.tsv")
ONLY=${SIFT_FAULT_ONLY:-}
declare -A MINE   # the operations of this slice: the dearest first, each to the slice with the least cost so far (seconds a row: scope_cic_nj 20, col_promote 4, vacuum and col_roll 2, the others 1)
while read -r op; do MINE[$op]=1; done < <(printf '%s\n' "${ROWS[@]}" | awk -F'\t' '
	BEGIN { w["scope_cic_nj"] = 20; w["col_promote"] = 4; w["vacuum"] = 2; w["col_roll"] = 2 }
	{ c[$2] += ($2 in w) ? w[$2] : 1 }
	END { for (x in c) print c[x] "\t" x }' | sort -t$'\t' -k1,1nr -k2,2 | awk -F'\t' -v k="${SLICE%/*}" -v n="${SLICE#*/}" '
	{ m = 1; for (i = 2; i <= n; i++) if (l[i] < l[m]) m = i; l[m] += $1; if (m == k) print $2 }')
total=0
for row in "${ROWS[@]}"; do
	IFS=$'\t' read -r id name phase fault tier mutant expect <<<"$row"
	[[ -z $ONLY || $id =~ $ONLY ]] || continue
	[[ -v MINE[$name] ]] || continue
	[[ -n ${KCOUNT[$name]:-} ]] || measure "$name"
	if [[ $phase != gate:* ]] && (( ${KCOUNT[$name]} == 0 )); then
		(( ++total )); skip 1 "$id: the operation publishes no directory update, its phases are the gates"
		continue
	fi
	(( ++total ))
	fresh "${SETUP[$name]}" || bail "$id: setup $(sql cm "SELECT 1" 2>&1 | head -2)"
	[[ $DAMAGED == *" $name "* ]] || healthy || bail "$id: the starting state is not healthy"
	HELD_INTERRUPTS= TAMPER_LATE= SKIP_MAINTAIN= ORPHAN_WAIT=
	if ! run_fault "$name" "$phase" "$fault"; then
		if [[ -n $HELD_INTERRUPTS ]]; then
			skip 1 "$id: the process holds interrupts where it waits: a $fault does not act there"
			fresh "" >/dev/null 2>&1
		elif [[ $phase == b* ]] && ! grep -q "corrupt" "$D/fault.out"; then
			skip 1 "$id: this run makes fewer directory updates than the clean run counted: the update is not reached"
		elif [[ $phase == a* ]] && ! grep -q "corrupt" "$D/fault.out"; then
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
done_testing
finish
