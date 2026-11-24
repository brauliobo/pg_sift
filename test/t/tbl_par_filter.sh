#!/usr/bin/env bash
# The filtered aggregate of a sift_table on the compute threads of the backend (col/f2/pin_win.h): a granule whose rows are all there for the snapshot goes to the threads with the predicates
# of the statement; a granule a pending delete of another transaction can touch, a snapshot that is serializable or a state the stored chunks cannot judge goes through the scan as it always
# did. Every answer is compared with the heap executor on the same table (sift.enable_agg = off), and with a heap twin loaded with the same rows, under the states another session leaves:
# uncommitted deletes, inserts and updates, those rolled back, and those committed after the snapshot of the statement.
. "$(dirname "$0")/lib.bash"

plan 14
node_init pf 63 "autovacuum = off"
node_start pf || bail "start failed"
D=$(dir pf)
SET=("SET max_parallel_workers_per_gather = 3" "SET work_mem = '256MB'" "SET sift.gpu = off")
sql pf "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
sql pf "SET sift.test_create_format1 = off" \
	"CREATE TABLE pf (id int, g int, h smallint, v bigint, n int, d date) USING sift_table" \
	"CREATE TABLE ph (id int, g int, h smallint, v bigint, n int, d date)" \
	"INSERT INTO pf SELECT i, i % 7, i % 3, i * 3, CASE WHEN i % 10 = 0 THEN NULL ELSE i % 11 END, date '2024-01-01' + (i % 30) FROM generate_series(1, 300000) i" \
	"INSERT INTO ph SELECT * FROM pf" "VACUUM (FREEZE, ANALYZE) pf" "ANALYZE ph" >/dev/null || bail "load failed"

QD="SELECT g, count(DISTINCT v) FROM pf WHERE v >= 3000 AND h = 0 AND n IS NOT NULL AND d >= '2024-01-05' GROUP BY g ORDER BY g"
QN="SELECT count(DISTINCT v) FROM pf WHERE n IS NULL AND g IN (1, 3, 5)"
QALL="SELECT count(DISTINCT v) FROM pf"
DML=("DELETE FROM TT WHERE id % 13 = 0" "UPDATE TT SET v = v + 1, n = 3 WHERE id % 17 = 0"
	"INSERT INTO TT SELECT i, 2, 0, i * 3, 2, date '2024-02-01' FROM generate_series(310001, 311000) i")

# the statements under the threads (DEBUG1 says how many granules they took), under the heap executor, and on the heap twin
thr() { sql pf "${SET[@]}" "SET client_min_messages = debug1" "$@" 2>&1; }
rows() { grep -v DEBUG; }
took() { local n; n=$(sed -n 's/.*sift agg v6: \([0-9]*\) granules on [0-9]* threads, folded by the threads/\1/p' | tail -1); echo "${n:-0}"; }
heap() { sql pf "${SET[@]}" "SET sift.enable_agg = off" "$1"; }
ref() { sql pf "${SET[@]}" "${1//FROM pf/FROM ph}"; }
held() { # STATEMENT...: another transaction runs them and stays open until release
	rm -f "$D/held" "$D/release"
	{ echo "BEGIN;"; printf '%s;\n' "$@"; echo "\\! touch '$D/held'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "ROLLBACK;"; } | sql pf >/dev/null &
	HOLDER=$!
	wait_for "[[ -e $D/held ]]" 60 || bail "the holding transaction did not start"
}
release() { touch "$D/release"; wait "$HOLDER"; }

ALL=$(thr "$QALL" | took)
check "the settled table has granules for the threads" test "${ALL:-0}" -ge 16
out=$(thr "$QD")
FILTERED=$(took <<<"$out")
check "a filtered distinct count runs on the threads, on the granules the scan does not prune" test "${FILTERED:-0}" -ge 16 -a "${FILTERED:-0}" -le "$ALL"
check "and answers as the heap does" test "$(rows <<<"$out")" == "$(heap "$QD")"
out=$(thr "$QN")
check "a null test and an IN list run on the threads" test "$(took <<<"$out")" == "$ALL" -a "$(rows <<<"$out")" == "$(ref "$QN")"

held "DELETE FROM pf WHERE id <= 100" "INSERT INTO pf SELECT i, 1, 0, i * 3, 1, date '2024-02-01' FROM generate_series(300001, 301000) i" "UPDATE pf SET v = v + 7 WHERE id BETWEEN 100000 AND 100100"
out=$(thr "$QD")
check "uncommitted deletes, inserts and updates: no granule they can touch goes to the threads" test "$(took <<<"$out")" -lt "$FILTERED"
check "and the rows are those of the heap: the other transaction's changes are not seen" test "$(rows <<<"$out")" == "$(ref "$QD")"
out=$(thr "$QALL")
check "an unfiltered count is bounded the same way" test "$(took <<<"$out")" -lt "$ALL" -a "$(rows <<<"$out")" == "$(heap "$QALL")"
release
out=$(thr "$QD")
check "rolled back: the rows are those of the heap" test "$(rows <<<"$out")" == "$(ref "$QD")"

# the snapshot of a REPEATABLE READ transaction is older than what another session commits meanwhile
other() { echo "\\! psql -X -At -q -v ON_ERROR_STOP=1 -h '$D/sock' -p '$(port pf)' -U postgres -d postgres -c \"${1//TT/pf}\""; }
script() { # QUERY: the answers of the threads and of the heap executor, to the snapshot of the first statement
	printf '%s;\n' "${SET[@]}" "SET client_min_messages = debug1" "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT count(*) FROM pf"
	for s in "${DML[@]}"; do other "$s"; done
	printf '%s\n' "\\echo THREADS" "$1;" "SET sift.enable_agg = off;" "\\echo HEAP" "$1;" "COMMIT;"
}
snapshot() { # QUERY: THREADS and HEAP answers agree, say no error, and the threads took none of the granules the commits touched
	local got
	got=$(script "$1" | psql -X -At -q -h "$D/sock" -p "$(port pf)" -U postgres -d postgres 2>&1)
	! grep -q ERROR <<<"$got" && (( $(sed -n '/^THREADS/,/^HEAP/p' <<<"$got" | took) < FILTERED )) && [[ $(sed -n '/^THREADS/,/^HEAP/p' <<<"$got" | rows | sed '/^THREADS$/d;/^HEAP$/d') == "$(sed -n '/^HEAP/,$p' <<<"$got" | rows | sed 1d)" ]]
}
check "deletes, updates and inserts committed after the snapshot: the threads see the rows of the snapshot" snapshot "$QD"
for s in "${DML[@]}"; do sql pf "${s//TT/ph}" >/dev/null; done
out=$(thr "$QD")
check "a statement after them sees the committed state, as the heap and its twin do" test "$(rows <<<"$out")" == "$(heap "$QD")" -a "$(rows <<<"$out")" == "$(ref "$QD")"
out=$(thr "$QN")
check "a null test and an IN list too" test "$(rows <<<"$out")" == "$(ref "$QN")"
sql pf "BEGIN" "INSERT INTO pf SELECT i, 4, 0, i, 4, date '2024-03-01' FROM generate_series(400001, 401000) i" "DELETE FROM pf WHERE id % 5 = 0" "ROLLBACK" >/dev/null
out=$(thr "$QD")
check "an aborted transaction leaves no trace" test "$(rows <<<"$out")" == "$(ref "$QD")"
sql pf "VACUUM pf" "VACUUM ph" >/dev/null
out=$(thr "$QD")
check "after VACUUM the answer is the same" test "$(rows <<<"$out")" == "$(ref "$QD")" -a "$(took <<<"$out")" -ge 1
out=$(thr "BEGIN ISOLATION LEVEL SERIALIZABLE" "$QD" "COMMIT")
check "a serializable snapshot reads through the scan" test "$(took <<<"$out")" == 0 -a "$(rows <<<"$out")" == "$(ref "$QD")"
finish
