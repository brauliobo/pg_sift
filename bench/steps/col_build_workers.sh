#!/usr/bin/env bash
# CREATE INDEX .. USING sift_col at 0..N parallel maintenance workers, default reloptions: wall time, the IO of the build, and whether a build with workers is slower than the serial one.
#
#   PGHOST=... PGPORT=... PGUSER=... PGDATABASE=db bench/steps/col_build_workers.sh [-t TABLE] [-k "COLUMNS"] [-w "0 2 4"] [-r REPS]
#
# Prints one TSV line per worker count: fastest wall ms of REPS builds (the host is shared: a median moves by 30%), blocks hit and read, MB read, WAL MB, index MB (steps/iolib.py, of the last build). A count whose median
# is over the serial one by more than 10% is marked SLOWER and the script ends with status 1. Reduced data (1,000,000 rows): the table needs no more.
set -euo pipefail
export LC_ALL=C
HERE=$(dirname "$0")
TABLE=events COLS="tenant_id, dim_a_id, event_date, status, kind, counter_a, value_num" WORKERS="0 2 4" REPS=3
while getopts t:k:w:r: opt; do
	case $opt in t) TABLE=$OPTARG ;; k) COLS=$OPTARG ;; w) WORKERS=$OPTARG ;; r) REPS=$OPTARG ;; *) sed -n '2,8p' "$0"; exit 1 ;; esac
done
DB=${PGDATABASE:-postgres}
q() { psql -X -v ON_ERROR_STOP=1 -qAt -d "$DB" "$@"; }

printf 'workers\twall_ms\thit\tread\tread_mb\twal_mb\tindex_mb\n'
base= bad=0
for w in $WORKERS; do
	set_="SET jit = off; SET maintenance_work_mem = '1GB'; SET min_parallel_table_scan_size = 0; SET max_parallel_maintenance_workers = $w;"
	create="CREATE INDEX col_build_workers ON $TABLE USING sift_col ($COLS)"
	walls=()
	for ((i = 0; i < REPS; i++)); do
		q -c "DROP INDEX IF EXISTS col_build_workers" >/dev/null 2>&1
		s=$(date +%s%N)
		q -c "$set_ $create" >/dev/null 2>&1
		walls+=($(((($(date +%s%N) - s)) / 1000000)))
	done
	q -c "DROP INDEX IF EXISTS col_build_workers" >/dev/null 2>&1
	io=$(python3 -I "$HERE/iolib.py" --db "$DB" --size "$TABLE" --json "$set_ $create")
	med=$(printf '%s\n' "${walls[@]}" | sort -n | head -1)
	[[ -n $base ]] || base=$med
	flag=; if ((med * 100 > base * 110)); then flag=SLOWER; bad=1; fi
	jq -r --arg w "$w" --arg ms "$med" --arg f "$flag" '[$w, $ms, .hit, .read, (.read_bytes / 1048576 | round), (.wal_bytes / 1048576 | round), (.size_bytes / 1048576 * 10 | round / 10), $f] | @tsv' <<<"$io"
done
q -c "DROP INDEX IF EXISTS col_build_workers" >/dev/null
exit $bad
