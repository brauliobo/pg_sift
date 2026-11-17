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

FAMILIES=(bm25_promote bm25_merge col_promote col_merge scope_build scope_refill vacuum tbl_seal)
SCENARIOS+=(
	"scope_cic~$GP~CREATE INDEX CONCURRENTLY docs_sc ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, scope = 't: g -> gp(g, t)')"
	"reindex_col~$COL~REINDEX INDEX docs_col"
)
FAMILIES+=(scope_cic reindex_col)
declare -A SETUP OP
for row in "${SCENARIOS[@]}"; do IFS='~' read -r name setup op <<<"$row"; SETUP[$name]=$setup; OP[$name]=$op; done

on_standby() { # the checks of a healthy node on the standby: indexes verify, store check, answers equal REF through the indexes and without
	local plain idx
	sql sb "$VERIFY" >&2 || return 1
	plain=$(sql sb "${PLAIN[@]}" "$QA" "$QB" 2>&1)
	[[ $plain == "$REF" ]] || { diag "the plain answers of the standby are not the primary's: $plain"; return 1; }
	idx=$(sql sb "${IDX[@]}" "$QA" "$QB" 2>&1)
	[[ $idx == "$plain" ]] || { diag "the answers through the indexes of the standby differ from its plain plan"; return 1; }
}
catch_up() { catchup cm sb; }
restart_primary() { node_stop cm; pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1; node_start cm; }

for name in "${FAMILIES[@]}"; do
	[[ -z ${SIFT_FAULT_ONLY:-} || $name =~ $SIFT_FAULT_ONLY ]] || continue
	fresh "${SETUP[$name]}" || bail "$name: setup"
	REF=$(sql cm "${PLAIN[@]}" "$QA" "$QB" 2>&1)
	k=$(sql cm "${OP[$name]}" "SELECT sift._test_dir_updates()" | tail -1) || bail "$name: the operation failed in a clean state"
	(( k > 0 )) || continue
	for n in $(( (k + 1) / 2 )) "-$k"; do
		for kind in 1 0; do
			fresh "${SETUP[$name]}" || bail "$name: setup"
			REF=$(sql cm "${PLAIN[@]}" "$QA" "$QB" 2>&1)
			sql cm "SET sift.test_crash_dir = $n" "SET sift.test_crash_kind = $kind" "${OP[$name]}" >/dev/null 2>&1
			if [[ $kind == 0 ]]; then
				for _ in $(seq 100); do kill -0 "$(head -1 "$D/data/postmaster.pid" 2>/dev/null)" 2>/dev/null || break; sleep 0.1; done
				node_start cm || { not_ok "$name, update $n, kind $kind: the primary did not restart"; diag "$(tail -20 "$D/postgres.log")"; continue; }
			fi
			for _ in 1 2 3; do # an INVALID index (a build that was stopped) is dropped as an operator does, the one another depends on after it
				for ix in $(sql cm "SELECT i.indexrelid::regclass FROM pg_index i WHERE NOT i.indisvalid"); do sql cm "DROP INDEX CONCURRENTLY $ix" >/dev/null 2>&1; done
			done
			catch_up
			if on_standby; then
				sql cm "${OP[$name]}" >/dev/null && catch_up && on_standby && ok "$name, update $n, $([[ $kind == 1 ]] && echo error || echo kill): the standby verifies, answers equal, and follows the operation run again" ||
					not_ok "$name, update $n, kind $kind: after the operation run again"
			else
				not_ok "$name, update $n, kind $kind: on the standby after the fault"
			fi
		done
	done
done

node_promote sb
wait_for "[[ \$(sql sb \"SELECT pg_is_in_recovery()\") == f ]]" 60 || bail "the standby did not promote"
if sql sb "$VERIFY" >&2; then ok "the promoted standby verifies every index"; else not_ok "the promoted standby does not verify"; fi
echo "1..$NTEST"
finish
