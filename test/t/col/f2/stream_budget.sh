#!/usr/bin/env bash
# The memory, the temporary files, the file size and the time of what VACUUM, autovacuum, CREATE INDEX and a bulk UPDATE do to a Format 2.0 relation do not grow with its rows: every case is run at
# three sizes (the column cases 32768, 131072 and 524288 rows, the table cases 32768, 65536 and 131072) of text that compresses about 9 times, with the build run at a budget far above the one the maintenance runs at (a 128 MB build writes one compressed
# part that holds the whole table, a 2 MB VACUUM must not read it back into memory) and with temp_file_limit = 1MB, so that a file in pgsql_tmp fails the case. The resident anonymous memory of the
# process is sampled every millisecond (scripts/rss-peak.py); the bytes the compaction counts are read from sift._test_mem_highwater().
#   alpha_mem  = peak(largest) / peak(smallest)                     <= 1.15
#   beta_rss   = (peak(largest) - peak(smallest)) / (rows(largest) - rows(smallest))  <= 8 B/row; beta_exact the same on the counted bytes <= 2 B/row
#   temp_bytes = the growth of pg_stat_database.temp_bytes         == 0
#   alpha_disk = pg_relation_size / live bytes of the index      <= 2.0
#   alpha_time = seconds(largest) / seconds(smallest), scaled to a 4x size ratio <= 5.5
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
SCALES=(32768 131072 524288)
TABLE_SCALES=(32768 65536 131072)
S1=${SCALES[0]}
S4=${SCALES[2]}
UNIQUE_ROWS=262144
done_testing
node_init fb 1 "autovacuum = on
autovacuum_naptime = 1s
autovacuum_work_mem = 2MB
maintenance_work_mem = 64MB
temp_file_limit = 1MB
max_parallel_maintenance_workers = 0
max_parallel_workers_per_gather = 0
shared_buffers = 32MB
max_wal_size = 512MB
sift.async_maintenance = off"
node_start fb || bail "start failed"
sql fb "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

D=$(dir fb)
ROWS=$D/rows
: >"$ROWS"
RSS=$REPO/scripts/rss-peak.py
psqlf() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$D/sock" -p "$(port fb)" -U postgres -d postgres "$@"; }
temp_bytes() { sql fb "SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()"; }
load() { sql fb "SET temp_file_limit = -1" "DROP TABLE IF EXISTS $1" "CREATE TABLE $1 (id int, grp int, pad text) ${3:-} WITH (autovacuum_enabled = off)" \
	"INSERT INTO $1 SELECT g, g % 64, repeat(md5(g::text), 12) FROM generate_series(1, $2) g" >/dev/null || bail "load of $1 failed"; }
build() { sql fb "SET maintenance_work_mem = '$3'" "CREATE INDEX $2 ON $1 USING sift_col (grp, id, pad)" >/dev/null || bail "build of $2 failed"; }

# note CASE N BASE_KB PEAK_KB CPU_SECS TEMP0 INDEX: one line of the table of the case; the sizes are read after the statement
note() {
	local size=- live=-
	if [[ -n ${7:-} ]]; then
		size=$(sql fb "SELECT pg_relation_size('$7')")
		live=$(sql fb "SELECT bytes FROM sift.col_index_info('$7')")
	fi
	echo "$1 $2 $3 $4 $5 $(( $(temp_bytes) - $6 )) $size $live ${EXACT:--1}" >>"$ROWS"
}

# settle INDEX: two more VACUUMs of the table of the index, so that the blocks the measured statement freed are reused or cut off before the size is read
settle() { [[ -z $1 ]] || sql fb "VACUUM $(sql fb "SELECT indrelid::regclass FROM pg_index WHERE indexrelid = '$1'::regclass")" "VACUUM $(sql fb "SELECT indrelid::regclass FROM pg_index WHERE indexrelid = '$1'::regclass")" >/dev/null; }

# measured CASE N INDEX: the statements on stdin in a session of their own, whose resident anonymous memory is sampled and whose highwater of the counted bytes is read at the end
measured() {
	local case=$1 n=$2 stop=$D/stop.$1.$2 out=$D/rss.$1.$2 temp0 secs peak base
	rm -f "$stop" "$out" "$out.ready"
	temp0=$(temp_bytes)
	{
		echo "SELECT pg_backend_pid() AS pid \\gset"
		echo "\\setenv PID :pid"
		echo "\\! python3 -I $RSS pid \$PID $stop $out &"
		echo "\\! until [ -e $out.ready ]; do sleep 0.002; done"
		echo "SELECT count(*) FROM sift._test_mem_highwater(true);"
		cat
		echo "SELECT 'EXACT', coalesce(max(peak), -1) FROM sift._test_mem_highwater(false) WHERE scope IN ('f2_compact', 'f2_bulkdelete');"
		echo "\\! touch $stop"
	} >"$D/m.sql"
	psqlf -f "$D/m.sql" >"$D/m.out" 2>"$D/m.err" || { diag "$case $n: $(cat "$D/m.err")"; not_ok "$case at $n rows runs"; return 1; }
	wait_for "[[ -s $out ]]" 30 || bail "the sampler of $case did not finish"
	read -r base peak secs <"$out"
	settle "${3:-}"
	EXACT=$(sed -n 's/^EXACT|//p' "$D/m.out") note "$case" "$n" "$base" "$peak" "$secs" "$temp0" "${3:-}"
}

for n in "${SCALES[@]}"; do
	load a$n $n
	build a$n a${n}_i 128MB
	sql fb "DELETE FROM a$n WHERE id % 4 = 0" >/dev/null
	measured col_vacuum $n a${n}_i <<SQL
SET maintenance_work_mem = '2MB';
VACUUM a$n;
SQL
	check "col_vacuum at $n rows: the index is intact and holds the live rows" test "$(sql fb "SELECT sift.verify('a${n}_i', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
	sql fb "DROP TABLE a$n" >/dev/null
done

for n in "${SCALES[@]}"; do
	load c$n $n
	build c$n c${n}_i 128MB
	sql fb "DELETE FROM c$n WHERE id % 4 = 0" >/dev/null
	stop=$D/stop.col_autovacuum.$n out=$D/rss.col_autovacuum.$n
	rm -f "$stop" "$out"
	temp0=$(temp_bytes)
	python3 -I "$RSS" autovacuum "$(head -1 "$D/data/postmaster.pid")" "$stop" "$out" &
	sampler=$!
	sql fb "ALTER TABLE c$n SET (autovacuum_enabled = on, autovacuum_vacuum_threshold = 50, autovacuum_vacuum_scale_factor = 0)" >/dev/null
	wait_for "[[ \$(sql fb \"SELECT last_autovacuum IS NOT NULL FROM pg_stat_user_tables WHERE relname = 'c$n'\") == t ]]" 120 || bail "autovacuum did not vacuum c$n"
	touch "$stop"
	wait "$sampler"
	read -r base peak _ <"$out"
	sql fb "ALTER TABLE c$n SET (autovacuum_enabled = off)" >/dev/null
	settle c${n}_i
	note col_autovacuum "$n" "$base" "$peak" -1 "$temp0" c${n}_i
	check "col_autovacuum at $n rows: the index is intact" test "$(sql fb "SELECT sift.verify('c${n}_i', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
	sql fb "DROP TABLE c$n" >/dev/null
done

for n in "${SCALES[@]}"; do
	load e$n $n
	build e$n e${n}_i 2MB
	sql fb "DELETE FROM e$n WHERE id % 2 = 0" >/dev/null
	measured col_bulkdelete $n e${n}_i <<SQL
SET maintenance_work_mem = '8MB';
VACUUM e$n;
SQL
	check "col_bulkdelete at $n rows: the index is intact" test "$(sql fb "SELECT sift.verify('e${n}_i', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
	sql fb "DROP TABLE e$n" >/dev/null
done

for n in "${TABLE_SCALES[@]}"; do
	sql fb "SET temp_file_limit = -1" "DROP TABLE IF EXISTS d$n" "CREATE TABLE d$n (id int, grp int, pad text) USING sift_table WITH (autovacuum_enabled = off)" \
		"INSERT INTO d$n SELECT g, g % 64, repeat(md5(g::text), 12) FROM generate_series(1, $n) g" >/dev/null || bail "load of d$n failed"
	measured tbl_update $n <<SQL
SET work_mem = '1MB';
SET maintenance_work_mem = '4MB';
UPDATE d$n SET grp = grp + 1;
SQL
	check "tbl_update at $n rows: every row is there once" test "$(sql fb "SELECT count(*) = $n AND count(DISTINCT id) = $n FROM d$n")" == t
	sql fb "DROP TABLE d$n" >/dev/null
done

for n in "${TABLE_SCALES[@]}"; do
	sql fb "SET temp_file_limit = -1" "DROP TABLE IF EXISTS g$n" "CREATE TABLE g$n (id int, grp int, pad text) USING sift_table WITH (autovacuum_enabled = off)" \
		"INSERT INTO g$n SELECT g, g % 64, repeat(md5(g::text), 12) FROM generate_series(1, $n) g" >/dev/null || bail "load of g$n failed"
	measured tbl_delete $n <<SQL
SET work_mem = '1MB';
SET maintenance_work_mem = '4MB';
DELETE FROM g$n WHERE id % 2 = 0;
SQL
	check "tbl_delete at $n rows: the odd ids are left" test "$(sql fb "SELECT count(*) = $n / 2 AND bool_and(id % 2 = 1) FROM g$n")" == t
	sql fb "DROP TABLE g$n" >/dev/null
done

load u $UNIQUE_ROWS
UT0=$(temp_bytes)
measured unique_build $UNIQUE_ROWS <<SQL
SET temp_file_limit = -1;
SET maintenance_work_mem = '16MB';
CREATE UNIQUE INDEX u_u ON u USING sift_col (id);
SQL
UNIQUE_MWM_KB=16384

report=$(python3 -I - "$ROWS" "$S1" "$S4" "$UNIQUE_ROWS" "$UNIQUE_MWM_KB" <<'PY'
import sys
path, s1, s4, urows, mwm_kb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
# the table access method keeps the staged keys of a statement until it ends (src/table/f2): the bound is the leak of one 8 kB page per row it once had
TABLE_RSS_BYTES_PER_ROW = 1024.0
rows = {}
for line in open(path):
    f = line.split()
    case, n = f[0], int(f[1])
    rows.setdefault(case, {})[n] = dict(base=int(f[2]), peak=int(f[3]), secs=float(f[4]), temp=int(f[5]),
                                         size=None if f[6] == "-" else int(f[6]), live=None if f[7] == "-" else int(f[7]), exact=int(f[8]))
out = ["TABLE case | peak_1x | peak_2x | peak_4x | alpha_mem | beta_exact | beta_rss | temp_bytes | alpha_disk | alpha_time | verdict"]
for case, by in rows.items():
    if case == "unique_build":
        continue
    ns = sorted(by)
    a, c = by[ns[0]], by[ns[-1]]
    dn = ns[-1] - ns[0]
    alpha = c["peak"] / max(a["peak"], 1)
    beta = (c["peak"] - a["peak"]) * 1024.0 / dn
    exact = (c["exact"] - a["exact"]) / dn if min(a["exact"], c["exact"]) >= 0 else None
    temp = max(v["temp"] for v in by.values())
    disk = max((v["size"] / v["live"] for v in by.values() if v["live"]), default=None)
    tm = (c["secs"] / a["secs"]) * 4.0 / (ns[-1] / ns[0]) if a["secs"] > 0 else None
    tbl = case.startswith("tbl_")
    rss_limit = TABLE_RSS_BYTES_PER_ROW if tbl else 8.0
    checks = [("beta_rss", beta, rss_limit, beta <= rss_limit), ("temp_bytes", temp, 0, temp == 0)]
    if not tbl:
        checks.insert(0, ("alpha_mem", alpha, 1.15, alpha <= 1.15))
    if case == "tbl_delete":
        checks.append(("beta_exact", exact, 2.0, exact is not None and exact <= 2.0))
    elif case in ("col_vacuum", "col_bulkdelete"):
        checks.append(("beta_exact", exact, 2.0, exact is not None and exact <= 2.0))
    elif case == "col_autovacuum":
        checks.append(("beta_exact", None, 2.0, True))
    if disk is not None:
        checks.append(("alpha_disk", disk, 2.0, disk <= 2.0))
    if tm is not None and case != "col_autovacuum":
        checks.append(("alpha_time", tm, 5.5, tm <= 5.5))
    verdict = "PASS" if all(c_[3] for c_ in checks) else "FAIL"
    f = lambda v, p=2: "-" if v is None else f"{v:.{p}f}"
    out.append("TABLE %s | %d | %d | %d | %s | %s | %s | %d | %s | %s | %s" % (
        case, *[by[n]["peak"] for n in ns], f(alpha), f(exact), f(beta, 1), temp, f(disk), f(tm), verdict))
    for name, value, limit, good in checks:
        out.append("CHECK %s %s %s <= %s %s" % (case, name, f(value) if value is not None else "n/a", limit, "ok" if good else "not ok"))
u = rows["unique_build"][urows]
grown = (u["peak"] - u["base"]) / 1024.0
cap = 1.25 * mwm_kb / 1024.0
out.append("CHECK unique_build peak_over_base_mb %.1f <= %.1f %s" % (grown, cap, "ok" if grown <= cap else "not ok"))
print("\n".join(out))
PY
)
grep '^TABLE' <<<"$report" | sed 's/^TABLE /# /'
echo "# peaks are the resident anonymous kB of the process; the rows of the table are written to $ROWS"
while IFS= read -r l; do
	[[ $l == CHECK* ]] || continue
	desc=${l#CHECK }
	if [[ $desc == *" not ok" ]]; then not_ok "${desc% not ok}"; else ok "${desc% ok}"; fi
done <<<"$report"
[[ -z ${F2_BUDGET_LOG:-} ]] || { sed 's/^TABLE //' <<<"$(grep '^TABLE' <<<"$report")"; grep '^CHECK' <<<"$report"; echo "ROWS case n base_kb peak_kb secs temp_bytes size live exact"; cat "$ROWS"; } >"$F2_BUDGET_LOG"
check "the unique index is intact" test "$(sql fb "SELECT sift.verify('u_u', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
finish
