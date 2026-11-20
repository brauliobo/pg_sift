#!/usr/bin/env bash
# The exact basis of the memory slope gate (scripts/mem-slope.py, BASIS column of test/mem-slope.tsv): a row on the exact basis passes or fails on the bytes the memory scopes
# of sift counted (sift._test_mem_highwater()), whatever the resident gauge reads. The merge of the gate's table holds the string dictionary of its partition writer, a slope of tens of
# bytes a row: its row recorded at exact + max(2 B/row, 15%) passes, the same case with a ceiling lower by that allowance (a leak that outgrew its record) fails, and so does it
# under the clean ceiling; an update that holds nothing stays on its clean ceiling. Every ceiling is derived from the exact slope this run measured, no timer and no sampler decides.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init mx 39 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.col_auto_merge = off"
node_start mx || bail "start failed"
sql mx "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

slope() { PGHOST=$(dir mx)/sock PGPORT=$(port mx) PGUSER=postgres PGDATABASE=postgres MEM_SLOPE_RUNS=1 MEM_SLOPE_TSV="$1" python3 -I "$REPO/scripts/mem-slope.py" run --rows 30000 2>"$(dir mx)/slope.err"; }
row() { awk -F'\t' -v OFS='\t' -v src="$1" -v name="$2" -v ceiling="$3" -v basis="$4" -v status="$5" '$1 == src { $1 = name; $3 = ceiling; $4 = basis; $5 = status; print }' "$REPO/test/mem-slope.tsv"; }

row merge probe 1000 exact open >"$(dir mx)/probe.tsv"
probe=$(slope "$(dir mx)/probe.tsv" | grep '^SLOPE probe ')
exact=$(sed -E 's/.*memory scopes (-?[0-9.]+) B\/row.*/\1/' <<<"$probe")
at=$(awk -v e="$exact" 'BEGIN { a = e * 0.15; if (a < 2) a = 2; x = e + a; print (x == int(x)) ? x : int(x) + 1 }')
under=$(awk -v e="$exact" 'BEGIN { a = e * 0.15; if (a < 2) a = 2; print int(e - a) }')

{
	row merge merge_at "$at" exact open
	row merge merge_under "$under" exact open
	row merge merge_clean 12 exact clean
	row update_all update_flat 12 exact clean
} >"$(dir mx)/cases.tsv"
out=$(slope "$(dir mx)/cases.tsv")

diag "$probe"
diag "$(grep -E '^SCOPES ' <<<"$out")"
sum_of() { awk -v n="$1" '$1 == "SCOPES" && $2 == n { for (i = 1; i <= NF; i++) if ($i == "B/row") s += $(i - 1) } END { printf "%.1f", s }' <<<"$out"; }
under_sum=$(sum_of merge_under)
plan 10
check "the merge holds a slope that follows its rows ($exact B/row of memory scopes)" test "${exact%.*}" -ge 20
check "the case recorded at its exact slope plus the allowance ($at B/row) passes" grep -q '^SLOPE merge_at .* on exact PASS$' <<<"$out"
check "the same case with a ceiling of $under B/row, the allowance under its exact slope, fails" grep -q '^SLOPE merge_under .* on exact FAIL$' <<<"$out"
check "the same case under the clean ceiling fails" grep -q '^SLOPE merge_clean .* on exact FAIL$' <<<"$out"
check "an update that holds nothing passes the clean ceiling" grep -q '^SLOPE update_flat .* on exact PASS$' <<<"$out"
check "the failing case prints the scopes that make its slope: they add up to its exact slope ($under_sum of $exact B/row)" awk -v a="$under_sum" -v b="$exact" 'BEGIN { exit !(a - b < 1.5 && b - a < 1.5) }'
check "the failing case names at least two scopes" test "$(grep -c '^SCOPES merge_under ' <<<"$out")" -ge 2
check "the cases that pass print no scopes" test "$(grep -c '^SCOPES \(merge_at\|update_flat\) ' <<<"$out")" -eq 0
check "no case ended in an error" test -z "$(grep -h ERROR <<<"$out")"
check "the harness read no error of the server" test ! -s "$(dir mx)/slope.err"
finish
