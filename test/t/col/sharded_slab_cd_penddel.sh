#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The writers of ONE partition scale. (1) 16 sessions that insert one row at a time into the single partition of a sift_table and of a sift_col index, with a reader beside them, do at least
# 4 times the work of one session, lose no row to a crash that follows at once, and the table and the index check clean. (2) 16 sessions that update a column no index holds (the per-column
# update pages of the partition) while 2 sessions delete rows of the same partition in blocks that fold the pending deletes do at least 5 times the work of one, none of them waits
# longer than 50 ms for the fold, and the sums are exact. The table and the index are of Format 2.0 on both runs. Writers that each keep a CPU busy cannot do more than the CPUs allow, so
# the factors are at most 0.6 times the cores (CORES): 2.4 on a runner of 4, the whole 4 and 5 from 7 and 9 cores up.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 12
SECS=$(knob SIFT_S103_SECS 3 2)
UROWS=20000
DROWS=40000
node_init sc 1 "autovacuum = off
shared_preload_libraries = 'pg_sift'
max_connections = 80${SIFT_TESTING:+
sift.test_create_format1 = off}"
node_start sc || bail "start failed"
D=$(dir sc)
sql sc "CREATE EXTENSION pg_sift" \
	"CREATE TABLE ti (id bigserial, v int) USING sift_table WITH (autovacuum_enabled = false)" \
	"CREATE TABLE hc (id bigserial, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX hc_idx ON hc USING sift_col (id, v)" \
	"CREATE TABLE cd (id bigint, a bigint, b text, c text, d text, e text) USING sift_table WITH (autovacuum_enabled = false)" \
	"INSERT INTO cd SELECT g, 0, repeat('b', 40), repeat('c', 40), repeat('d', 40), repeat('e', 40) FROM generate_series(1, $UROWS) g" \
	"INSERT INTO cd SELECT 100000 + g, g, repeat('b', 40), repeat('c', 40), repeat('d', 40), repeat('e', 40) FROM generate_series(1, $DROWS) g" \
	"VACUUM cd" >/dev/null || bail "setup failed"
echo "INSERT INTO ti (v) VALUES (1);" >"$D/ins_ti.sql"
echo "INSERT INTO hc (v) VALUES (1);" >"$D/ins_hc.sql"
printf '\\set rid random(1, %s)\nUPDATE cd SET a = a + 1 WHERE id = :rid;\n' "$UROWS" >"$D/upd.sql"
printf '\\set lo 100001 + random(0, %s)\nDELETE FROM cd WHERE id BETWEEN :lo AND :lo + 199;\n' "$((DROWS - 200))" >"$D/del.sql"

tps_of() { awk '/^tps = /{printf "%d", $3}' "$1"; }
txns_of() { awk '/transactions actually processed/{print $NF}' "$1"; }
ratio_ge() { awk -v a="$1" -v b="$2" -v m="$3" 'BEGIN{exit !(b > 0 && a / b >= m)}'; }
scale() { awk -v f="$1" -v c="$CORES" 'BEGIN{m = 0.6 * c; printf "%.1f", f < m ? f : m}'; }
INS_X=$(scale 4.0)
UPD_X=$(scale 5.0)

reader() {
	while [[ ! -e $D/stop ]]; do sql sc "SELECT count(*) FROM $1" >/dev/null 2>>"$D/reader.err" || echo failed >>"$D/reader.err"; done
}

# insert_run SCRIPT CLIENTS TABLE: pgbench, a reader on the table beside it; the TPS and the rows it committed
insert_run() {
	rm -f "$D/stop"
	reader "$3" & rp=$!
	pgbench_bg sc "$2" "$SECS" -f "$D/$1"
	wait "$PGBENCH_PID"
	touch "$D/stop"
	wait "$rp"
	cp "$D/pgbench.log" "$D/run_$1_$2.log"
	TPS=$(tps_of "$D/pgbench.log")
	TXNS=$(txns_of "$D/pgbench.log")
}

: >"$D/reader.err"
insert_run ins_ti.sql 1 ti; ti1=$TPS; ti1_rows=$TXNS
insert_run ins_ti.sql 16 ti; ti16=$TPS; ti_rows=$((ti1_rows + TXNS))
diag "sift_table single-row INSERT: 1 writer $ti1 tps, 16 writers $ti16 tps"
check "sift_table: 16 writers of one partition do >= ${INS_X}x the work of 1 ($ti16 vs $ti1 tps)" ratio_ge "$ti16" "$ti1" "$INS_X"
insert_run ins_hc.sql 1 hc; hc1=$TPS; hc1_rows=$TXNS
insert_run ins_hc.sql 16 hc; hc16=$TPS; hc_rows=$((hc1_rows + TXNS))
diag "sift_col single-row INSERT: 1 writer $hc1 tps, 16 writers $hc16 tps"
check "sift_col: 16 writers of one partition do >= ${INS_X}x the work of 1 ($hc16 vs $hc1 tps)" ratio_ge "$hc16" "$hc1" "$INS_X"
check "the readers met no error" test ! -s "$D/reader.err"

crash sc
node_start sc || bail "restart after the crash failed"
check "sift_table: after the crash every committed row is there ($ti_rows)" test "$(sql sc "SELECT count(*) FROM ti")" == "$ti_rows"
check "sift_col: after the crash every committed row is there ($hc_rows)" test "$(sql sc "SELECT count(*) FROM hc")" == "$hc_rows"
check "sift.check finds nothing in the table and the index" test "$(sql sc "SELECT count(*) FROM sift.check('ti'::regclass)" "SELECT count(*) FROM sift.check('hc_idx'::regclass, true)" | tr '\n' ' ')" == "0 0 "
check "the column scan counts what the heap does" test "$(sql sc "SET sift.enable_agg = on" "SELECT count(*), sum(v) FROM hc" | tail -1)" == "$hc_rows|$hc_rows"

# update_run CLIENTS: updaters with the 2 deleters beside them; the TPS, the transactions and the updates that waited longer than 50 ms
update_run() {
	rm -f "$D"/lat.*
	pgbench -n -h "$D/sock" -p "$(port sc)" -U postgres -c 2 -j 2 -T "$SECS" -f "$D/del.sql" postgres >"$D/del.log" 2>&1 &
	dp=$!
	pgbench_bg sc "$1" "$SECS" -f "$D/upd.sql" -l --log-prefix="$D/lat"
	wait "$PGBENCH_PID"
	wait "$dp"
	TPS=$(tps_of "$D/pgbench.log")
	TXNS=$(txns_of "$D/pgbench.log")
	STALLS=$(cat "$D"/lat.* | awk '$3 > 50000 {n++} END {print n + 0}')
}
update_run 1; up1=$TPS; up_txns=$TXNS
update_run 16; up16=$TPS; up_txns=$((up_txns + TXNS)); stalls=$STALLS
diag "col_delta UPDATE beside 2 block deleters: 1 writer $up1 tps, 16 writers $up16 tps, $stalls updates over 50 ms"
check "16 updaters do >= ${UPD_X}x the work of 1 ($up16 vs $up1 tps)" ratio_ge "$up16" "$up1" "$UPD_X"
check "no update of the 16 waited more than 50 ms for the fold of the pending deletes ($stalls)" test "$stalls" -eq 0
check "the updates and the deletes ended without error" test "$(grep -ac -E '^(pgbench: error|ERROR)|failed transactions: [1-9]' "$D/pgbench.log" "$D/del.log" | awk -F: '{n += $NF} END {print n + 0}')" -eq 0
check "the sum of the updated column is exact ($up_txns updates)" test "$(sql sc "SELECT sum(a) FROM cd WHERE id <= $UROWS")" == "$up_txns"
check "sift.check finds nothing in the updated table" test "$(sql sc "SELECT count(*) FROM sift.check('cd'::regclass)")" == 0
finish
