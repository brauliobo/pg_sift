#!/usr/bin/env bash
# The log of a column index that tracks its changes names the triggers it was made with (the identity of their catalog rows). pg_upgrade, a restore and ENABLE TRIGGER ALL after DISABLE TRIGGER ALL
# make the triggers again, a concurrent build makes the log before the triggers hear a statement, and the log is then not taken for complete: the reads check the heap (right, slower) until it is
# re-baselined. The worker that a read of such an index asks for (also VACUUM, and the worker that a write asks for) re-baselines it, enabling the triggers for all sessions again first when
# ENABLE TRIGGER left them for the origin only. Here: a real pg_upgrade of a cluster whose indexes the library of an earlier release wrote (damaged, flags 1), from the previous major when
# its binaries are installed (SIFT_PG_OLD_BIN, or the PGDG directory of the major before this one) and from this one otherwise, then the other flows on the upgraded cluster; the heap-free
# read, the checked read and the heap give the same answer at every step.
#
# pg_upgrade loads '$libdir/pg_sift' in both clusters, which the staged install (scripts/pgtest.sh) is not in: each side runs from a copy of its
# binaries at the same absolute path under the test directory (a server finds its library and share directories relative to its binaries), with the
# files of the system linked in and pg_sift of the staged install, or of the build for the other major, next to them. The clusters have data
# checksums, which PostgreSQL 18 enables by default and pg_upgrade needs on both sides.
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/lib_upgrade.bash"

NEWCFG=$(command -v pg_config)
OLDBIN=${SIFT_PG_OLD_BIN:-}
if [[ -z $OLDBIN ]]; then
	for c in "/usr/lib/postgresql/$((PG_MAJOR - 1))/bin" "/usr/pgsql-$((PG_MAJOR - 1))/bin"; do
		[[ -x $c/pg_config ]] && { OLDBIN=$c; break; }
	done
fi
# SIFT_PG_OLD_REF: the old cluster runs a released library (a tag or a release branch, built for the old major from its archive) and the objects it wrote with
# the options it has; after pg_upgrade the extension is updated to this tree's version as a user does. Unset: this tree on both sides.
OLD_REF=${SIFT_PG_OLD_REF:-v4.1.0}
if [[ -n $OLD_REF ]]; then
	git -C "$REPO" rev-parse --verify -q "$OLD_REF^{commit}" >/dev/null || { echo "1..0 # SKIP $OLD_REF is not in this repository"; exit 0; }
	[[ $(uname -m) == x86_64 || $(printf '%s\n' "${OLD_REF#v}" 3.2.0 | sort -V | head -1) == 3.2.0 ]] || { echo "1..0 # SKIP $OLD_REF builds on x86-64 only"; exit 0; }
fi
OLDCFG=${OLDBIN:+$OLDBIN/pg_config}
OLDCFG=${OLDCFG:-$NEWCFG}
OLD_MAJOR=$("$OLDCFG" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
# a server image can carry a major pg_sift does not support (the Makefile's PG_MIN_MAJOR): the upgrade then runs within the major
if ((OLD_MAJOR < $(sed -n 's/^PG_MIN_MAJOR *= *\([0-9]*\).*/\1/p' "$REPO/Makefile" | head -n 1))); then OLDCFG=$NEWCFG; OLD_MAJOR=$PG_MAJOR; fi

TS=$TAPDIR/tablespace
OV=$TAPDIR/overlay
trap 'pg_ctl -D "$(dir old)/data" -m immediate stop >/dev/null 2>&1; cleanup; ((NFAIL)) || rm -rf "$(dir old)" "$OV" "$TS" "$TAPDIR/old-src"' EXIT
rm -rf "$TS" "$OV"
mkdir -p "$TS" "$OV"
diag "pg_upgrade from PostgreSQL $OLD_MAJOR to $PG_MAJOR"

# the servers: this major from the staged install, the old one from the same or a build of this tree for its major
overlay "$NEWCFG" "$OV/new" "$ROOT"
if ((OLD_MAJOR == PG_MAJOR)) && [[ -z $OLD_REF ]]; then
	overlay "$OLDCFG" "$OV/old" "$ROOT"
else
	build_old "$OLDCFG" "$OV/oldroot" || { (($? == 77)) && exit 0; tail -n 20 "$TAPDIR/old-build.log"; bail "pg_sift does not build for PostgreSQL $OLD_MAJOR"; }
	overlay "$OLDCFG" "$OV/old" "$OV/oldroot"
fi
plan 16
OLDB=$OV/old$("$OLDCFG" --bindir)
NEWB=$OV/new$("$NEWCFG" --bindir)
PATH=$NEWB:$PATH

initdb_node old 30 "$OLDB"
initdb_node new 31 "$NEWB"
NODES+=(new)
node_conf new "$("$REPO/scripts/pgtest.sh" conf)"
LD_PRELOAD=${SIFT_PRELOAD:-} "$OLDB/pg_ctl" -D "$(dir old)/data" -l "$(dir old)/postgres.log" -w -t 120 start >/dev/null 2>&1 || bail "the old cluster does not start"


TBLS="ta tb tc td"
SETUP=$(cat <<SQL
CREATE EXTENSION pg_sift;
CREATE FUNCTION one() RETURNS int LANGUAGE plpgsql AS \$\$ BEGIN PERFORM 1; RETURN 0; END \$\$;
CREATE TABLE ta (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, pad text NOT NULL DEFAULT repeat('x', 80)) WITH (autovacuum_enabled = false, fillfactor = 100);
CREATE TABLE tb (LIKE ta INCLUDING ALL) WITH (autovacuum_enabled = false, fillfactor = 100);
CREATE TABLE tc (LIKE ta INCLUDING ALL) WITH (autovacuum_enabled = false, fillfactor = 100);
CREATE TABLE td (LIKE ta INCLUDING ALL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO ta SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i;
INSERT INTO tb SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i;
INSERT INTO tc SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i;
INSERT INTO td SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i;
CREATE INDEX ta_c ON ta USING sift_col (code, tenant, amount);
CREATE INDEX tb_c ON tb USING sift_col (code, tenant, amount);
CREATE INDEX tc_c ON tc USING sift_col (code, tenant, amount) WITH (write_buffer_limit = 64);
CREATE INDEX td_c ON td USING sift_col (code, tenant, amount);
VACUUM (FREEZE, ANALYZE) ta;
VACUUM (FREEZE, ANALYZE) tb;
VACUUM (FREEZE, ANALYZE) tc;
VACUUM (FREEZE, ANALYZE) td;
SQL
)
qs old postgres <<<"$SETUP" >/dev/null || bail "the old cluster cannot be set up"
"$OLDB/pg_ctl" -D "$(dir old)/data" -m fast -w stop >/dev/null 2>&1
LD_PRELOAD=${SIFT_PRELOAD:-} "$OLDB/pg_ctl" -D "$(dir old)/data" -l "$(dir old)/postgres.log" -w -t 120 start >/dev/null 2>&1 || bail "the old cluster does not restart"
# a new session each: the UPDATE is its first statement (the attach of the shared tables inside it); a function that runs SQL inside the UPDATE; a table written many times
q old postgres "BEGIN" "UPDATE ta SET amount = amount + 1 WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i)" "UPDATE ta SET amount = amount + 5 WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i)" "COMMIT" >/dev/null
q old postgres "BEGIN" "UPDATE tb SET amount = amount + 1 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i)" "UPDATE tb SET amount = amount + 5 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i)" "COMMIT" >/dev/null
q old postgres "BEGIN" "UPDATE tc SET amount = amount + 1 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i)" "COMMIT" >/dev/null
q old postgres "INSERT INTO td SELECT 'n' || i, i % 10, i FROM generate_series(1, 3000) i" >/dev/null
# QS TABLE: the grouped answer
QS() { echo "SELECT string_agg(tenant || ':' || n || ':' || s, ' ' ORDER BY tenant) FROM (SELECT tenant, count(*) n, sum(amount) s FROM $1 GROUP BY tenant) q"; }
# ans TABLE MODE: free (the heap-free read), checked (the heap is checked for the rows that need it), heap (the normal plan)
ans() {
	local on=on nh=off
	[[ $2 == heap ]] && on=off
	[[ $2 == checked ]] && nh=on
	q new postgres "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $on" "SET sift.test_no_heapfree = $nh" "$(QS $1)" | tail -n 1
}
differential() { local t; for t in "$@"; do [[ $(ans "$t" free) == $(ans "$t" heap) && $(ans "$t" checked) == $(ans "$t" heap) ]] || return 1; done; }
state() { q new postgres "SELECT cur_flags || ' ' || (SELECT heap_free FROM sift.col_tracking('$1')) FROM sift._test_log_chan('$1')"; }
tg() { q new postgres "SELECT string_agg(tgenabled::text, ',') FROM pg_trigger WHERE tgrelid = '$1'::regclass AND tgname LIKE 'sift_track_%' AND tgisinternal"; }
flags_is() { [[ $(state "$1") == "$2" ]]; }
old_wrong=0
for t in ta tb; do [[ $(q old postgres "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "$(QS $t)" | tail -n 1) != $(q old postgres "SET sift.enable_agg = off" "$(QS $t)" | tail -n 1) ]] && old_wrong=$((old_wrong + 1)); done
check "the library of $OLD_REF counts row versions of two updated tables (the damage a re-baseline repairs)" test "$old_wrong" -eq 2
"$OLDB/pg_ctl" -D "$(dir old)/data" -m fast -w stop >/dev/null 2>&1 || bail "the old cluster does not stop"
UPG=("$NEWB/pg_upgrade" -b "$OLDB" -B "$NEWB" -d "$(dir old)/data" -D "$(dir new)/data" -U postgres -p "$(port old)" -P "$(port new)" -s "$(dir new)/sock")
(cd "$(dir new)" && LD_PRELOAD=${SIFT_PRELOAD:-} "${UPG[@]}" >"$(dir new)/pg_upgrade.log" 2>&1)
upgraded=$?
check "pg_upgrade of the cluster succeeds" test $upgraded -eq 0
if [[ $upgraded -ne 0 ]]; then diag "$(tail -n 30 "$(dir new)/pg_upgrade.log")"; bail "pg_upgrade failed"; fi
node_start new || bail "the upgraded cluster does not start"
q new postgres "ALTER EXTENSION pg_sift UPDATE" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "the extension update failed"
unrepaired=0
for t in $TBLS; do [[ $(state ${t}_c) == "1 false" && $(q new postgres "SELECT triggers FROM sift.col_tracking('${t}_c')") == f ]] && unrepaired=$((unrepaired + 1)); done
check "after pg_upgrade the four logs have the flags 1, and the recreated triggers are not the ones they name" test "$unrepaired" -eq 4
check "... the reads check the heap and answer as the heap (the heap-free request, the checked read)" differential $TBLS
q new postgres "ALTER SYSTEM SET sift.col_auto_merge = on" "SELECT pg_reload_conf()" >/dev/null
sleep 1
# the log is consulted by a read of rows that VACUUM has not marked all-visible: the tables hold the updates of the old cluster
for t in $TBLS; do q new postgres "SET sift.enable_agg = on" "$(QS $t)" >/dev/null; done
all_repaired() { local t; for t in $TBLS; do [[ $(state ${t}_c) == "3 true" ]] || return 1; done; }
check "one read of each table, and the worker re-baselines every log: flags 3, heap-free" wait_for all_repaired 600
check "... and the reads answer as the heap (the old versions of the old cluster are no longer counted)" differential $TBLS
check "... the triggers are the same two, enabled for all sessions" test "$(tg ta)" == "A,A"

# the other flows, on a table of the upgraded cluster
q new postgres "CREATE TABLE t (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, pad text NOT NULL DEFAULT repeat('x', 80)) WITH (autovacuum_enabled = false, fillfactor = 100)" \
	"INSERT INTO t SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i" "CREATE INDEX t_c ON t USING sift_col (code, tenant, amount)" "VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "the table t cannot be made"
churn() { q new postgres "UPDATE t SET amount = amount + 1 WHERE code IN (SELECT 'c' || i FROM generate_series($1, 20000, 7) i)" "DELETE FROM t WHERE code IN (SELECT 'c' || i FROM generate_series($2, 20000, 11) i)" >/dev/null; }
# triggers disabled: the writes are heard by no one; the user's decision stays, and the reads check the heap
q new postgres "ALTER TABLE t DISABLE TRIGGER ALL" >/dev/null
churn 7 5
q new postgres "SET sift.enable_agg = on" "$(QS t)" >/dev/null
sleep 4
is() { [[ $(tg "$1") == "$2" && $(state "$3") == "$4" ]] && differential "$1"; }
check "triggers disabled by the user, writes meanwhile: the triggers stay disabled, the log is not re-baselined, the reads answer as the heap" is t D,D t_c "3 false"
# enabled again (for the origin only): the worker enables them for all and re-baselines
q new postgres "ALTER TABLE t ENABLE TRIGGER ALL" >/dev/null
check "ENABLE TRIGGER ALL leaves them enabled for sessions that are not replicas only, and the reads answer as the heap" is t O,O t_c "3 false"
q new postgres "SET sift.enable_agg = on" "$(QS t)" >/dev/null
check "a read asks the worker: the triggers are enabled for all again, the log is re-baselined (heap-free), the answers are the heap's" wait_for 'is t A,A t_c "3 true"' 600
# a concurrent rebuild: the log is made before the triggers hear a statement
q new postgres "REINDEX INDEX CONCURRENTLY t_c" >/dev/null
check "REINDEX CONCURRENTLY leaves a log that is not complete (flags 0), and the reads answer as the heap" is t A,A t_c "0 false"
churn 3 4
q new postgres "VACUUM t" >/dev/null
check "the next VACUUM re-baselines it, heap-free, and the answers are the heap's" is t A,A t_c "3 true"
# pg_restore --disable-triggers: a data-only restore into the table with its index
CONN="host=$(dir new)/sock port=$(port new) user=postgres dbname=postgres"
pg_dump -Fc -t t -d "$CONN" -f "$(dir new)/t.dump" || bail "pg_dump failed"
q new postgres "TRUNCATE t" "VACUUM (FREEZE, ANALYZE) t" >/dev/null
pg_restore -a --disable-triggers -d "$CONN" "$(dir new)/t.dump" >/dev/null 2>&1
check "pg_restore --disable-triggers leaves the triggers enabled for sessions that are not replicas only, and the reads answer as the heap" is t O,O t_c "3 false"
churn 6 8
q new postgres "SET sift.enable_agg = on" "$(QS t)" >/dev/null
check "a read asks the worker: the triggers are enabled for all, the log is re-baselined, the answers are the heap's" wait_for 'is t A,A t_c "3 true"' 600
# a table that was a partition (a statement on its parent fires none of its triggers)
q new postgres "CREATE TABLE pp (code text, tenant int NOT NULL, amount int NOT NULL, pad text) PARTITION BY RANGE (tenant)" "CREATE TABLE pp1 (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, pad text)" \
	"INSERT INTO pp1 SELECT 'c' || i, 1, i, repeat('x', 80) FROM generate_series(1, 20000) i" "CREATE INDEX pp1_c ON pp1 USING sift_col (code, tenant, amount)" "VACUUM (FREEZE, ANALYZE) pp1" \
	"ALTER TABLE pp ATTACH PARTITION pp1 FOR VALUES FROM (0) TO (5)" "DELETE FROM pp WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 9) i)" "ALTER TABLE pp DETACH PARTITION pp1" >/dev/null
check "a table that was a partition has a log that is not complete (flags 0), and the reads answer as the heap" is pp1 A,A pp1_c "0 false"
q new postgres "VACUUM pp1" >/dev/null
check "the next VACUUM re-baselines it, and the answers are the heap's" is pp1 A,A pp1_c "3 true"
finish
