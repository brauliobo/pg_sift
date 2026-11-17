#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The fault matrix with a physical standby: for each operation family the primary is stopped in the middle and after the last directory update of the operation (sift.test_crash_dir) by an
# ERROR and by a SIGKILL of the backend (the server restarts and replays its WAL), the standby replays everything, and on the standby every sift index verifies (heapallindexed), the
# store check passes and the answers through the indexes equal the plain plan's and what the table held before; the operation runs again on the primary and the standby follows. The
# standby is promoted at the end and the same checks pass on it. SIFT_FAULT_ONLY (a regex of the operation names) picks the families.
HERE=$(dirname "$0")
. "$HERE/lib.bash"

node_init cm 1 "wal_consistency_checking = 'generic'
autovacuum = off
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
shared_buffers = 64MB"
node_start cm || bail "start failed"
D=$(dir cm)
SIFT_TMP=$D leak_expect 400 'terminated by signal 9|terminating connection'
sql cm "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
. "$HERE/crash_scenarios.bash"
node_clone cm sb 2 "sift.async_maintenance = off" || bail "standby failed"
node_start sb || bail "standby start failed"

FAMILIES=(bm25_promote bm25_merge col_promote col_merge scope_build scope_refill vacuum tbl_seal scope_cic reindex_col reindex_scope_c vs_intern vs_compact tbl_copy alter_refresh)
GATE_FAMILIES=(tbl_tomb:tombwrite tbl_tomb:tombmerge tbl_seal:tombrewrite vs_intern:intern tbl_copy:load)
declare -A SETUP OP
for row in "${SCENARIOS[@]}"; do IFS='~' read -r name setup op <<<"$row"; SETUP[$name]=$setup; OP[$name]=$op; done

on_standby() { # the checks of a healthy node on the standby: indexes verify, store check, answers equal REF through the indexes and without
	local plain idx
	sql sb "$VERIFY" >&2 || return 1
	plain=$(sql sb "${PLAIN[@]}" "$QA" "$QB" 2>&1)
	[[ $plain == "$REF" ]] || { diag "the plain answers of the standby are not the primary's: $plain"; return 1; }
	idx=$(sql sb "${IDX[@]}" "$QA" "$QB" 2>&1)
	[[ $idx == "$plain" ]] || { diag "the answers through the indexes of the standby differ from its plain plan"; return 1; }
	[[ $(sql sb "SELECT count(*) FROM pg_class WHERE relname = 'tbl'") == 0 ]] ||
		[[ $(sql sb "$QT tbl") == "$(sql cm "$QT tbl_h")" ]] || { diag "the sift_table of the standby differs from the heap twin of the primary"; return 1; }
}
catch_up() { catchup cm sb; }
restart_primary() { node_stop cm; pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1; node_start cm; }

for name in "${FAMILIES[@]}"; do
	[[ -z ${SIFT_FAULT_ONLY:-} || $name =~ $SIFT_FAULT_ONLY ]] || continue
	fresh "${SETUP[$name]}" || bail "$name: setup"
	REF=$(sql cm "${PLAIN[@]}" "$QA" "$QB" 2>&1)
	k=$(op_stmts "$name"; sql cm "${OPS[@]}" "SELECT sift._test_dir_updates()" | tail -1) || bail "$name: the operation failed in a clean state"
	(( k > 0 )) || continue
	for n in $(( (k + 1) / 2 )) "-$k"; do
		for kind in 1 0; do
			fresh "${SETUP[$name]}" || bail "$name: setup"
			REF=$(sql cm "${PLAIN[@]}" "$QA" "$QB" 2>&1)
			sql cm "SET sift.test_crash_dir = $n" "SET sift.test_crash_kind = $kind" "${OPS[@]}" >/dev/null 2>&1
			if [[ $kind == 0 ]]; then
				for _ in $(seq 100); do kill -0 "$(head -1 "$D/data/postmaster.pid" 2>/dev/null)" 2>/dev/null || break; sleep 0.1; done
				node_start cm || { not_ok "$name, update $n, kind $kind: the primary did not restart"; diag "$(tail -20 "$D/postgres.log")"; continue; }
			fi
			for _ in 1 2 3; do # an INVALID index (a build that was stopped) is dropped as an operator does, the one another depends on after it
				for ix in $(sql cm "SELECT i.indexrelid::regclass FROM pg_index i WHERE NOT i.indisvalid"); do sql cm "DROP INDEX CONCURRENTLY $ix" >/dev/null 2>&1; done
			done
			catch_up
			if on_standby; then
				op_stmts "$name"; sql cm "${OPS[@]}" >/dev/null && catch_up && on_standby && ok "$name, update $n, $([[ $kind == 1 ]] && echo error || echo kill): the standby verifies, answers equal, and follows the operation run again" ||
					not_ok "$name, update $n, kind $kind: after the operation run again"
			else
				not_ok "$name, update $n, kind $kind: on the standby after the fault"
			fi
		done
	done
done

# the operations with no directory update of their own, stopped at their gates (a cancel, an immediate shutdown of the primary)
for row in "${GATE_FAMILIES[@]}"; do
	name=${row%:*} gname=${row#*:}
	[[ -z ${SIFT_FAULT_ONLY:-} || $name =~ $SIFT_FAULT_ONLY ]] || continue
	for fault in cancel immediate; do
		fresh "${SETUP[$name]}" || bail "$name: setup"
		REF=$(sql cm "${PLAIN[@]}" "$QA" "$QB" 2>&1)
		gate_spec "$gname"
		sql cm "SELECT $GATE_LOCK, pg_sleep(600)" >/dev/null 2>&1 &
		hold=$!
		wait_for "[[ -n \$(sql cm \"SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND granted\") ]]" 30 || bail "$name: the gate was not taken"
		op_stmts "$name"
		sql cm "${GATE_SET[@]}" "${OPS[@]}" >/dev/null 2>&1 &
		op=$!
		wait_for "[[ -n \$(sql cm \"SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND NOT granted\") ]]" 60 || { not_ok "$name, gate $gname: not reached"; continue; }
		if [[ $fault == cancel ]]; then
			sql cm "SELECT pg_cancel_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND NOT granted" >/dev/null
			wait "$op" 2>/dev/null
			sql cm "SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND $GATE_COND AND granted" >/dev/null
		else
			pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1
			node_start cm || { not_ok "$name, gate $gname: the primary did not restart"; continue; }
		fi
		wait 2>/dev/null
		catch_up
		if on_standby; then
			sql cm "${OPS[@]}" >/dev/null && catch_up && on_standby && ok "$name, gate $gname, $fault: the standby verifies, answers equal, and follows the operation run again" ||
				not_ok "$name, gate $gname, $fault: after the operation run again"
		else
			not_ok "$name, gate $gname, $fault: on the standby after the fault"
		fi
	done
done

node_promote sb
wait_for "[[ \$(sql sb \"SELECT pg_is_in_recovery()\") == f ]]" 60 || bail "the standby did not promote"
if sql sb "$VERIFY" >&2; then ok "the promoted standby verifies every index"; else not_ok "the promoted standby does not verify"; fi
echo "1..$NTEST"
finish
