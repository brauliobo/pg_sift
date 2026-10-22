#!/usr/bin/env bash
# Churn simulator for sift_col indexes: hours of insert/update/delete traffic compressed into cycles, a maintenance policy per arm, and the state of the
# index after every cycle (segments, dead rows, dirty units, pre-aggregate coverage, zone-map tightness, the latency of three query shapes, WAL and
# bytes written by each step). Every arm is a table with the same rows and receives the same DML; the answers of every arm must stay equal.
#
#   bench/churn/churn.sh run          build the master, run every arm, write bench/churn results to the tables of schema churn
#   bench/churn/churn.sh report       print the tables of the run (CSV on stdout)
#
# Connection: the usual PG* variables (PGHOST, PGPORT, PGUSER), database CHURN_DB (default churn, created if missing).
# Settings (environment): N rows (50000), CYCLES (6), RATE percent of the live rows touched per cycle (2), MIX upd:del:ins (40:30:30), HOT touch share:newest
# percent of the ids, e.g. 90:10 (0 = uniform), BATCH rows per transaction (200), ARMS (none worker force reindex), SCN name of the scenario (s1), QUERIES every
# cycle (1) or only the first and last (0). An arm may have index options (ARM_OPTS_<arm>, e.g. "merge_fanin = 4") and session settings for its
# VACUUM and maintenance steps (ARM_GUC_<arm>, e.g. "sift.col_auto_merge_dirty_pct=10 sift.maintenance_cost_delay=2").
#
# arms: none = VACUUM only (the built-in policy); worker = VACUUM + the automatic rewrite of sift.col_auto_merge (the session runs the rule the server's own
# worker applies, sift.col_maintenance_due(index, true), then the work, sift.col_maintain); force = VACUUM + sift.col_promote + sift.col_merge(index, true)
# every cycle; bgw = VACUUM and the server's own worker (set sift.col_auto_merge and sift.async_maintenance for the database, ALTER DATABASE: the worker reads no
# session setting, and then every arm of the database has them); reindex = VACUUM + REINDEX; any other name is a table with the options of ARM_OPTS_<name> that runs like worker with the settings of ARM_GUC_<name>.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DB=${CHURN_DB:-churn}
N=${N:-50000} CYCLES=${CYCLES:-6} RATE=${RATE:-2} MIX=${MIX:-40:30:30} HOT=${HOT:-0} BATCH=${BATCH:-200}
ARMS=${ARMS:-none worker force reindex} SCN=${SCN:-s1} QUERIES=${QUERIES:-1}
export PGOPTIONS="-c statement_timeout=60s"
q() { psql -X -q -At -d "$DB" -v ON_ERROR_STOP=1 "$@"; }

# arm_psql ARM SQL: a session with the settings of the arm
arm_psql() { local a=$1; shift; local g="ARM_GUC_$a" o=""; for kv in ${!g:-}; do o="$o -c $kv"; done; PGOPTIONS="$PGOPTIONS $o" psql -X -q -d "$DB" -v ON_ERROR_STOP=1 -c "$*" >/dev/null; }

# step ARM CYCLE NAME SQL: runs it, logs seconds, WAL bytes, the bytes of the segments it wrote and the load
step() {
	local a=$1 c=$2 name=$3; shift 3
	local ix=ev_${a}_col l0 l1 g0 t0 t1
	l0=$(q -c "select pg_current_wal_lsn()")
	g0=$(q -c "select coalesce(max(gen), 0) from sift.col_segments('$ix')" 2>/dev/null || echo 0)
	t0=$(date +%s.%N)
	arm_psql "$a" "$*"
	t1=$(date +%s.%N)
	l1=$(q -c "select pg_current_wal_lsn()")
	q -c "insert into churn.log values ('$SCN','$a',$c,'$name',$(echo "$t1 - $t0" | bc -l),(select pg_wal_lsn_diff('$l1','$l0')::int8),
		(select coalesce(sum(bytes), 0)::int8 from sift.col_segments('$ix') where gen > $g0),$(cut -d' ' -f1 /proc/loadavg))"
}

# maintenance of an arm after its VACUUM
maint() {
	local a=$1 c=$2 ix=ev_${1}_col
	case $a in
		none) ;;
		bgw) sleep 0.3; for _ in $(seq 300); do [[ $(q -c "select count(*) from pg_stat_activity where backend_type = 'sift maintenance'") == 0 ]] && break; sleep 0.1; done ;;
		force) step "$a" "$c" promote "select sift.col_promote('$ix')"; step "$a" "$c" merge "select sift.col_merge('$ix', true)" ;;
		reindex) step "$a" "$c" reindex "reindex index $ix" ;;
		*) step "$a" "$c" maintain "select case when sift.col_maintenance_due('$ix', true) is not null then sift.col_maintain('$ix') end" ;;
	esac
}

# measure under the perf lock when the host is quiet enough: the timed queries are the only thing that holds it
measure() {
	local a=$1 c=$2 stage=$3 queries=$4
	flock -o -w 3600 /tmp/sift-perf.lock nice psql -X -q -d "$DB" -v ON_ERROR_STOP=1 -c "set sift.async_maintenance = off" -c "select churn.measure('$SCN','$a',$c,'$stage',$queries)" >/dev/null
}

run() {
	createdb "$DB" 2>/dev/null || true
	q -c "create extension if not exists pg_sift" -c "create extension if not exists pg_visibility" -f "$HERE/lib.sql" -f "$HERE/dml.sql" >/dev/null
	q -c "delete from churn.state where scn = '$SCN'" -c "delete from churn.qres where scn = '$SCN'" -c "delete from churn.log where scn = '$SCN'"
	q -c "call churn.mkmaster($N)"
	local upd=${MIX%%:*} rest=${MIX#*:}; local del=${rest%%:*} ins=${rest#*:}
	local hot=${HOT%%:*} hotpct=10; [[ $HOT == *:* ]] && hotpct=${HOT#*:}
	for a in $ARMS; do
		local o="ARM_OPTS_$a"
		q -c "call churn.mkarm('$a', 100, '${!o:-}')"
		arm_psql "$a" "vacuum (analyze, freeze, disable_page_skipping) ev_$a"
		measure "$a" 0 base true
	done
	for c in $(seq 1 "$CYCLES"); do
		for a in $ARMS; do
			step "$a" "$c" dml "call churn.dml('$a', $c, $RATE, $upd, $del, $ins, $hot, $hotpct, $BATCH)"
			measure "$a" "$c" dml false
			step "$a" "$c" vacuum "vacuum (analyze) ev_$a"
			maint "$a" "$c"
			measure "$a" "$c" maint "$([[ $QUERIES == 1 || $c == $CYCLES ]] && echo true || echo false)"
		done
		local bad
		bad=$(q -c "select count(distinct churn.digest(a)) from unnest(string_to_array('$(echo $ARMS | tr ' ' ',')', ',')) a")
		[[ $bad == 1 ]] || echo "ANSWERS DIFFER between the arms in cycle $c" >&2
		for a in $ARMS; do q -c "select sift.col_verify('ev_${a}_col')" >/dev/null || echo "VERIFY FAILED $a $c" >&2; done
		echo "cycle $c done $(date +%T) load $(cut -d' ' -f1 /proc/loadavg)" >&2
	done
}

report() {
	psql -X -d "$DB" --csv -c "select * from churn.state where scn = '$SCN' order by arm, cycle, stage" -c "select * from churn.qres where scn = '$SCN' order by arm, cycle, q" \
		-c "select * from churn.log where scn = '$SCN' order by arm, cycle, step"
}

case ${1:-run} in run) run ;; report) report ;; *) sed -n '2,20p' "$0"; exit 1 ;; esac
