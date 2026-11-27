#!/usr/bin/env bash
# The creation, auxiliary maintenance, verification and storage-read paths of format major 2 hold what their budget lets them, not what the table is (test/t/col/f2/aux_reads_scale.py):
# a COPY and a CTAS of a sift_table, the pass that moves the end of the file, the build of a projection, the check of the keys and of every heap row, and the scan of a sift_table
# are each measured in a fresh backend at 1x, 2x and 4x rows with maintenance_work_mem 2 MB, work_mem 1 MB and temp_file_limit 1 MB over parts built with 64 MB. The tables are of Format 2.0 on both runs.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
python3 -I "$REPO/test/t/col/f2/aux_reads_scale.py" --selftest >/dev/null || bail "the selftest of the gate failed"
node_init as 41 "autovacuum = off
shared_buffers = 64MB
max_wal_size = 1GB
min_parallel_table_scan_size = 64kB${SIFT_TESTING:+
sift.test_create_format1 = off}"
node_start as || bail "start failed"
sql as "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

rows=$(knob SIFT_AUX_SCALE_ROWS 32768 32768)
out=$(PGHOST=$(dir as)/sock PGPORT=$(port as) PGUSER=postgres PGDATABASE=postgres AUX_SCALE_DIR=$(dir as)/files python3 -I "$REPO/test/t/col/f2/aux_reads_scale.py" --rows "$rows" 2>"$(dir as)/gate.err")
diag "$(grep -E '^GATE_' <<<"$out")"
cases=$(grep -c '^GATE_ROW' <<<"$out")
plan $((cases + 1))
check "the gate ran every case" test "$cases" -ge 9
while IFS= read -r line; do
	verdict=$(sed -E 's/.*\| (PASS|FAIL)( +#.*)?$/\1/' <<<"$line")
	check "${line#GATE_ROW }" test "$verdict" == PASS
done < <(grep '^GATE_ROW' <<<"$out")
finish
