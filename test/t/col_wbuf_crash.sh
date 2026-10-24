#!/usr/bin/env bash
# The write buffer lanes of the column engine under concurrent appenders (all lanes, page growth, pieces of big rows, sealing, VACUUM's
# flags), first undisturbed, then with kill -9 of the postmaster and every backend in the middle of the appends. After each crash and
# recovery: every row of a call that committed is there exactly once, every record matches its generator (checked in C), a row cut
# short by the crash is skipped, nothing is flagged dead that the VACUUM pass did not name, batches and records agree, sift_cwbuf_verify
# and the store check pass. Each round starts from a fresh index. SIFT_COLWBUF_ITERS (default 20, 2 quick).
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_COLWBUF_ITERS 20 2)
CLIENTS=8
plan $((ITERS + 1))
node_init cm 1 "checkpoint_timeout = 30s
max_wal_size = 256MB"
node_start cm || bail "start failed"
D=$(dir cm)

# one call of a client appends 40 rows with ids it alone owns and commits it with a row of done; a full segment is sealed;
# the limit is 512 kB of lane pages per segment
cat >"$D/append.sql" <<'SQL'
SELECT nextval('calls') * 40 AS lo \gset
BEGIN;
SELECT sift._test_col_wbuf_append('t_idx', 7, :lo, 40) AS crossed \gset
INSERT INTO done VALUES (:lo, 40);
END;
SELECT CASE WHEN :crossed > 0 THEN sift._test_col_wbuf_seal('t_idx') END;
SQL
echo "SELECT sift._test_col_wbuf_delete('t_idx', 7);" >"$D/delete.sql"
echo "CHECKPOINT;" >"$D/ckpt.sql"
ARGS=(-f "$D/append.sql@100" -f "$D/delete.sql@1" -f "$D/ckpt.sql@1")

sql cm "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
fresh() {
	sql cm "DROP TABLE IF EXISTS t, done CASCADE" "DROP SEQUENCE IF EXISTS calls" \
		"CREATE TABLE t (doc int)" "CREATE INDEX t_idx ON t USING sift_col (doc) WITH (write_buffer_limit = 512)" \
		"CREATE TABLE done (lo int, n int)" "CREATE SEQUENCE calls" \
		"SELECT sift._test_col_wbuf_prepare('t_idx')" >/dev/null
}

CHECK=("CREATE TEMP TABLE r AS SELECT * FROM sift._test_col_wbuf_rows('t_idx', 7)"
	"CREATE TEMP TABLE b AS SELECT * FROM sift._test_col_wbuf_batches('t_idx', 7)"
	"SELECT (SELECT count(*) FROM done d CROSS JOIN LATERAL generate_series(d.lo, d.lo + d.n - 1) g(id) LEFT JOIN r ON r.id = g.id
			 WHERE r.id IS NULL),
		(SELECT count(*) - count(DISTINCT id) FROM r),
		(SELECT count(*) FROM r WHERE deleted AND id % 7 <> 0),
		(SELECT count(*) FROM (SELECT id FROM r WHERE NOT deleted EXCEPT SELECT id FROM b) x) +
		(SELECT count(*) FROM (SELECT id FROM b EXCEPT SELECT id FROM r WHERE NOT deleted) x),
		coalesce((SELECT sum(live) FROM sift._test_col_wbuf_segments('t_idx')), 0) = (SELECT count(*) FROM r WHERE NOT deleted),
		sift._test_check('t_idx') >= 0,
		(SELECT count(*) FROM r), (SELECT count(*) FROM done), (SELECT count(*) FROM sift._test_col_wbuf_segments('t_idx')),
		(SELECT sum(nrows) FROM sift._test_col_wbuf_segments('t_idx')) - (SELECT count(*) FROM r)")

# healthy: missing | duplicated | wrongly flagged | records and batches differ | live agree | store check | records | calls | segments | torn rows
healthy() {
	local out
	out=$(sql cm "${CHECK[@]}" 2>&1)
	IFS='|' read -r missing dups wrong differ live store records calls segments torn <<<"$out"
	[[ $missing == 0 && $dups == 0 && $wrong == 0 && $differ == 0 && $live == t && $store == t ]] ||
		{ diag "$out"; return 1; }
	SUMMARY="$records records, $calls calls ($((records - calls * 40)) rows of calls that never committed), $segments segments, $torn torn"
}

fresh || bail "setup failed"
pgbench_bg cm "$CLIENTS" 2 "${ARGS[@]}"
if wait "$PGBENCH_PID" && healthy; then ok "undisturbed: $SUMMARY"; else not_ok "undisturbed"; diag "$(tail -5 "$D/pgbench.log")"; fi

for i in $(seq "$ITERS"); do
	fresh || bail "fresh index failed"
	crash_cycle cm "iteration $i" 2 "$CLIENTS" "${ARGS[@]}" || break
	if healthy; then ok "iteration $i: $SUMMARY"; else not_ok "iteration $i"; break; fi
done
finish
