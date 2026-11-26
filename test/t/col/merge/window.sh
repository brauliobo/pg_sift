#!/usr/bin/env bash
# tap: pooled
# A column merge holds the decoded units of a window, not a copy of each input: 12 inputs of about 17,000 rows at 1 MB of maintenance_work_mem keep the units of
# the inputs, the batch they are folded into and the segments open (scope col_merge_window, test/mem-budgets.tsv) under 11 MB (13.1 MB when every input kept the arrays of
# its largest unit), 48 inputs no more than 3 MB over that, and a merge cut by a key (the chunks of its inputs and the partition written, scope col_merge) under 44 MB
# (47.8 MB). The rows come out as they went in.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
# Format 1: the merge of Format 1 segments and the memory of its window of inputs
node_init mw 61 "sift.test_create_format1 = on
autovacuum = off
max_parallel_workers_per_gather = 0
sift.async_maintenance = off"
node_start mw || bail "start failed"
sql mw "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

ROWS=200000
ROW='g AS id, g % 7 AS k, g % 500 AS k2, (array[$$DE$$, $$FR$$, $$US$$, $$BR$$])[1 + g % 4] AS c, $$name$$ || (g::bigint * 7919) % 1000000 AS txt, ((g::bigint * 7919) % 29500)::numeric / 100 AS amount'
# table NAME INPUTS KEY_CUT: half of the rows in the build, the rest promoted in INPUTS segments
table() {
	local t=$1 k=$2 per=$((ROWS / 2 / $2 + 1)) i
	sql mw "CREATE TABLE $t AS SELECT $ROW FROM generate_series(1, $((ROWS / 2))) g" "ALTER TABLE $t ADD PRIMARY KEY (id)" \
		"CREATE INDEX ${t}_col ON $t USING sift_col (id, k, k2, c, txt, amount) WITH (key_cut = '$3')" "VACUUM ANALYZE $t" >/dev/null || bail "table $t failed"
	for ((i = 1; i <= k; i++)); do
		sql mw "INSERT INTO $t SELECT $ROW FROM generate_series($((ROWS / 2 + (i - 1) * per + 1)), $((ROWS / 2 + i * per < ROWS ? ROWS / 2 + i * per : ROWS))) g" \
			"SELECT sift.col_promote('${t}_col')" >/dev/null || bail "promotion $i of $t failed"
	done
}
peak() {
	sql mw "SET maintenance_work_mem = '1MB'" "SET max_parallel_maintenance_workers = 0" "SELECT sift.col_merge('$1', true)" \
		"SELECT peak FROM sift._test_mem_highwater() WHERE scope = '$2'" | tail -1
}
rows() { sql mw "SELECT count(*), sum(amount), count(DISTINCT txt), sum(k2) FROM $1"; }
verify_errors() { sql mw "SELECT sift.col_verify('$1', true)" 2>&1 | grep -c ERROR; }

table w12 12 off
check "the first table has 13 segments" test "$(sql mw "SELECT count(*) FROM sift.col_segments('w12_col') WHERE kind = 'immutable'")" -eq 13
before=$(rows w12)
p12=$(peak w12_col col_merge_window)
diag "12 inputs at 1 MB: $p12 bytes"
check "12 inputs hold under 11 MB at 1 MB of maintenance_work_mem" test "${p12:-0}" -gt 0 -a "${p12:-0}" -lt 11534336
check "the merged index verifies against the heap and holds the rows it held" test "$(verify_errors w12_col):$(rows w12)" == "0:$before"

table w48 48 off
p48=$(peak w48_col col_merge_window)
diag "48 inputs at 1 MB: $p48 bytes"
check "48 inputs hold no more than 3 MB over 12: the number of inputs does not show" test "${p48:-0}" -gt 0 -a "${p48:-0}" -lt $((${p12:-0} + 3145728))

table c12 12 k
pc=$(peak c12_col col_merge)
diag "12 inputs cut by a key at 1 MB: $pc bytes"
check "12 inputs cut by a key hold under 44 MB at 1 MB of maintenance_work_mem" test "${pc:-0}" -gt 0 -a "${pc:-0}" -lt 46137344
check "the index cut by a key verifies against the heap" test "$(verify_errors c12_col)" == 0

table r12 12 off
out=$(sql mw "SET maintenance_work_mem = '1MB'" "SET sift.test_mem_budget_scale = 0.09" "SELECT sift.col_merge('r12_col', true)" 2>&1) && out="merged"
check "a merge over its lowered budget fails naming col_merge_window" grep -qE "sift memory budget exceeded: col_merge_window: [0-9]+ > [0-9]+ bytes" <<<"$out"
finish
