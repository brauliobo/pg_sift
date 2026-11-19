#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A merge (sift.col_merge, and the maintenance at the end of a VACUUM) makes the sidecar of an entity segment again when more than sift.col_scope_refresh_pct of the rows of its runs sit in
# runs whose members' pairs changed since its stamp, and only when a sidecar made now would be stamped later. While ANOTHER session holds a pair change open the horizon stays where it was, a sidecar made
# under it is as stale as the one it replaces, and the step must end and write it at most once (it went on without end before): the call ends under a statement timeout, the log of the maintenance says
# it wrote the sidecar once, and the WAL of the call is that of one rewrite. When the session commits, the next step writes it once more, and one after that writes nothing. sift.col_maintain is the
# rewrite of the index: it ends too, and writes no sidecar unless a rewrite is due. The transaction that changed the pairs may itself merge and rewrite: the sidecar it makes is made from the pairs as
# committed (not its own changes, which roll back with it), and after its rollback every key reads as the join does. A SERIALIZABLE transaction takes no stamp (the journals refuse its snapshot), so a forced merge
# in it writes the merged segment without a sidecar, which the next merge step writes.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"

plan 27
node_init mo 67 "autovacuum = off
sift.col_auto_merge = off
sift.log_min_duration = 0
$SIDE_CONF"
node_start mo || bail "start failed"
D=$(dir mo)
side_setup mo || bail "setup failed"
side_facts mo 1 24000
sql mo "VACUUM (FREEZE, ANALYZE) events, tenant_scope" "CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)" >/dev/null || bail "build failed"
READ=$(side_read 1) JOIN=$(side_join 1)
check "the build wrote the sidecar and the read takes its runs" test "$(side_format mo events_col)" == 53 -a "$(side_runs mo "$READ")" -gt 0

sess() { rm -f "$D/$1.in"; mkfifo "$D/$1.in"; psql -X -At -q -h "$D/sock" -p "$(port mo)" -U postgres -d postgres <"$D/$1.in" >"$D/$1.out" 2>&1 & eval "exec {FD_$1}>\"$D/$1.in\""; }
say() { local v=FD_$1; echo "$2" >&"${!v}"; }
SEQ=0
hear() { SEQ=$((SEQ + 1)); say "$1" "\\echo sync$SEQ"; wait_for "grep -q '^sync$SEQ\$' '$D/$1.out'" 30 || bail "session $1 did not answer"; }
writes() { grep -c 'sift: \(wrote\|made\) the scope sidecar .*index "events_col"' "$D/postgres.log"; }
lsn() { sql mo "SELECT pg_current_wal_lsn()"; }
wal_since() { sql mo "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '$1')"; }
step() { # SQL: the call runs under a statement timeout; the writes of the sidecar it made and the WAL it wrote are in STEP_WRITES and STEP_WAL
	local w0=$(writes) l0=$(lsn)
	sql mo "SET statement_timeout = '120s'" "$1" >"$D/step.out" 2>&1
	STEP_RC=$?
	STEP_WRITES=$(($(writes) - w0))
	STEP_WAL=$(wal_since "$l0")
}
every_key() { local t; for t in 1 2 3; do side_same mo "$(side_read $t)" "$(side_join $t)" || return 1; done; }

# the change is the merging session's own: its snapshot sees it, the sidecar it makes is stamped with the horizon the open transaction holds back and is as stale as the one it replaces. The merge, the
# rewrite of sift.col_maintain (due: rows deleted and vacuumed leave dirty units) and the sidecars of the segments it writes end, and write each sidecar once
sql mo "DELETE FROM events WHERE id % 3 = 0" "VACUUM (INDEX_CLEANUP on) events" >/dev/null || bail "the deletes failed"
check "a rewrite is due (rows deleted and vacuumed)" test -n "$(sql mo "SELECT rewrite_due FROM sift.col_maintenance_status('events_col')")"
sess S
say S "SET statement_timeout = '120s';"
say S "BEGIN;"
say S "DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id % 5 IN (1, 2, 3);"
hear S
own_step() { # SQL: run in the open transaction of session S
	local w0=$(writes) l0=$(lsn)
	say S "$1"
	hear S
	STEP_ERR=$(grep -c ERROR "$D/S.out")
	STEP_WRITES=$(($(writes) - w0))
	STEP_WAL=$(wal_since "$l0")
}
say S "SELECT count(*) FILTER (WHERE valid), count(*) FROM sift._test_side_valid('events_col', 0, 1);"
hear S
OWN_WRITES=0
for _ in 1 2 3; do
	own_step "SELECT sift.col_merge('events_col') >= 0;"
	OWN_WRITES=$((OWN_WRITES + STEP_WRITES))
done
check "merges in the transaction that changed the pairs end, and wrote the sidecar once at most" test "$STEP_ERR" == 0 -a "$OWN_WRITES" -le 1
own_step "SELECT sift.col_maintain('events_col');"
SEGS=$(sql mo "SELECT count(*) FROM sift.col_segments('events_col') WHERE kind = 'immutable'")
check "the rewrite of sift.col_maintain in it ends, and wrote one sidecar at most for each segment it left ($STEP_WRITES of $SEGS)" test "$STEP_ERR" == 0 -a "$STEP_WRITES" -le "$SEGS"
say S "ROLLBACK;"
hear S
check "every key reads as the join does after it" every_key

# pair changes committed since the build touch the runs of key 1 and key 2: more than a tenth of their rows
sql mo "UPDATE tenant_scope SET tenant_id = 5 WHERE tenant_id = 1 AND dim_a_id % 5 = 0" "DELETE FROM tenant_scope WHERE tenant_id = 2 AND dim_a_id % 6 = 0" >/dev/null || bail "pair changes failed"
# another session changes the pairs of 20 members of key 1 and stays open
sess A
say A "BEGIN;"
say A "DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id % 5 = 1;"
hear A

step "SELECT sift.col_merge('events_col') >= 0"
check "a merge while another session holds a pair change open ends" test "$STEP_RC" == 0
MERGE_WRITES=$STEP_WRITES MERGE_WAL=$STEP_WAL
check "... and wrote the sidecar once (the horizon moved since the build) and not again" test "$MERGE_WRITES" == 1
step "SELECT sift.col_maintain('events_col') IS NOT NULL"
check "a call of sift.col_maintain while it is open ends" test "$STEP_RC" == 0 -a "$STEP_WRITES" == 0
step "SELECT sift.col_merge('events_col') >= 0"
check "a second merge writes no sidecar" test "$STEP_RC" == 0 -a "$STEP_WRITES" == 0
check "every key reads as the join does while the change is open" every_key
check "the index verifies" side_clean mo events_col

say A "COMMIT;"
hear A
step "SELECT sift.col_merge('events_col') >= 0"
check "after the commit a merge ends and writes the sidecar once" test "$STEP_RC" == 0 -a "$STEP_WRITES" == 1
check "the merge held by the open change wrote at most one rewrite of the WAL of the merge after the commit (and a few pages)" test "$MERGE_WAL" -le "$((STEP_WAL * 3 / 2 + 65536))"
step "SELECT sift.col_merge('events_col') >= 0"
check "the next merge writes nothing" test "$STEP_RC" == 0 -a "$STEP_WRITES" == 0
check "the read takes the runs again, with no run read by reference for the pair changes" test "$(side_runs mo "$READ")" -gt 0 -a "$(sql mo "SELECT count(*) FROM sift._test_side_valid('events_col', 0, 1) WHERE NOT valid")" == 0
check "every key reads as the join does" every_key

# the maintenance the end of a VACUUM runs (sift.col_auto_merge on) merges and writes the sidecar by the same rule
sql mo "ALTER SYSTEM SET sift.col_auto_merge = on" "SELECT pg_reload_conf()" >/dev/null
vacuum_step() { # the maintenance the end of a VACUUM runs
	local w0=$(writes) l0=$(lsn)
	sql mo "VACUUM events" >/dev/null || bail "VACUUM failed"
	STEP_WRITES=$(($(writes) - w0))
	STEP_WAL=$(wal_since "$l0")
}
sql mo "UPDATE tenant_scope SET tenant_id = 1 WHERE tenant_id = 5" "INSERT INTO tenant_scope SELECT m, 4 FROM generate_series(1, 60) m WHERE m % 3 = 0" >/dev/null || bail "pair changes failed"
say A "BEGIN;"
say A "DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id % 5 = 2;"
hear A
vacuum_step
check "the maintenance of a VACUUM, with another session's pair change open, wrote the sidecar once" test "$STEP_WRITES" == 1
vacuum_step
check "the next one wrote none" test "$STEP_WRITES" == 0
check "every key reads as the join does while the change is open" every_key
say A "COMMIT;"
hear A
vacuum_step
check "after the commit the VACUUM wrote the sidecar once" test "$STEP_WRITES" == 1
vacuum_step
check "the next one wrote none" test "$STEP_WRITES" == 0
check "the read takes the runs again" test "$(side_runs mo "$READ")" -gt 0
check "every key reads as the join does" every_key
check "the index verifies" side_clean mo events_col

# the journals cannot stamp a sidecar for a transaction that is SERIALIZABLE: a forced merge there writes its segment without the sidecar and the reads run the join or read by reference; the next merge step writes it
sql mo "ALTER SYSTEM SET sift.col_auto_merge = off" "SELECT pg_reload_conf()" >/dev/null
side_facts mo 24001 24600
sql mo "SELECT sift.col_promote('events_col') >= 0" >/dev/null || bail "promote failed"
sql mo "BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT sift.merge('events_col', true) >= 0; COMMIT" >/dev/null || bail "merge failed"
check "a forced merge in a SERIALIZABLE transaction leaves the merged segment without a sidecar" test "$(side_format mo events_col)" -lt 53
check "every key reads as the join does without it" every_key
sql mo "SELECT sift.merge('events_col') >= 0" >/dev/null
check "the next merge writes the sidecar and the read takes its runs" test "$(side_format mo events_col)" == 53 -a "$(side_runs mo "$READ")" -gt 0
finish
