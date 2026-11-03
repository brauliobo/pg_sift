# The order of the write buffer's rows that a backend keeps with the batches it read (col/scan/gorder.c) holds for the scans that open while the buffer changes. An ordered
# read opens, holds on the test gate while rows are deleted and VACUUM flags their records, the heap frees their addresses and HOT updates reuse them, or while rows are
# inserted (and one insert rolls back); then it reads the order it kept, extended with the rows appended: the answer is that of the heap at its snapshot, and the next scan reads the changes.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	CREATE TABLE iso_wo (id int PRIMARY KEY, k int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_wo SELECT g, (g * 7) % 41, 'x' FROM generate_series(1, 200) g;
	CREATE INDEX iso_wo_idx ON iso_wo USING sift_col (id, k) WITH (order_columns = 'k', order_array_columns = 'k');
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_wo;
}

# the rows of the write buffer, on the last page of the heap; the addresses of the last 30 are noted
setup
{
	INSERT INTO iso_wo SELECT 1000 + g, (g * 5) % 41, 'x' FROM generate_series(1, 60) g;
	CREATE TABLE iso_wo_gone AS SELECT ctid AS t FROM iso_wo WHERE id > 1030;
}

setup
{
	CREATE FUNCTION iso_wo_q(gate int, OUT n bigint, OUT agree bool, OUT ordered bool) LANGUAGE plpgsql AS $$
	DECLARE q text := 'SELECT count(*) FROM (SELECT id, k FROM iso_wo ORDER BY k, id LIMIT 300) s'; a text; b text; l text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		PERFORM set_config('sift.agg_force', 'on', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_sort', 'off', true);
		ordered := false;
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id, k FROM iso_wo ORDER BY k, id LIMIT 300' LOOP
			ordered := ordered OR l LIKE '%Global Order: true%';
		END LOOP;
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		EXECUTE 'SELECT string_agg(id || '':'' || k, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, id, k FROM (SELECT id, k FROM iso_wo ORDER BY k, id LIMIT 300) s) s' INTO a;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'off', true);
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_sort', 'on', true);
		EXECUTE 'SELECT string_agg(id || '':'' || k, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, id, k FROM (SELECT id, k FROM iso_wo ORDER BY k, id LIMIT 300) s) s' INTO b;
		agree := a = b;
		n := array_length(string_to_array(b, ','), 1);
	END $$;
}

teardown
{
	DROP TABLE iso_wo, iso_wo_gone;
	DROP FUNCTION iso_wo_q(int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_wo; }
step s1w	{ SELECT n, agree, ordered FROM iso_wo_q(0); }
step s1q	{ SELECT n, agree, ordered FROM iso_wo_q(7002); }
step s1m	{ SELECT hits > 0 AS order_found, extends > 0 AS order_extended FROM sift._test_col_gkept_stats(); }
step s1c	{ COMMIT; }
step s1z	{ SELECT n, agree, ordered FROM iso_wo_q(0); }

session s2
step s2d	{ DELETE FROM iso_wo WHERE id > 1030; }
step s2l	{ SELECT pg_advisory_lock(7002); }
step s2v	{ VACUUM iso_wo; }
step s2h	{ UPDATE iso_wo SET pad = 'y' WHERE id BETWEEN 1001 AND 1030; }
step s2r	{ SELECT count(*) AS reused FROM iso_wo w JOIN iso_wo_gone g ON w.ctid = g.t; }
step s2u	{ SELECT pg_advisory_unlock(7002); }
step s2i	{ INSERT INTO iso_wo SELECT 2000 + g, (g * 3) % 41, 'x' FROM generate_series(1, 40) g; }
step s2a	{ BEGIN; INSERT INTO iso_wo SELECT 3000 + g, g % 7, 'x' FROM generate_series(1, 40) g; ROLLBACK; }
step s2q	{ SELECT n, agree, ordered FROM iso_wo_q(0); }
step s2x	{ SELECT sift.col_verify('iso_wo_idx')::text = '' AS verified, (SELECT sum(rows) FROM sift.col_segments('iso_wo_idx')) AS index_rows, (SELECT count(*) FROM iso_wo) AS heap_rows; }

# the backend keeps the order; rows are deleted; a snapshot is taken, the ordered read opens, VACUUM flags the records of the deleted rows, the heap frees their addresses
# and the HOT updates of the rows next to them reuse them; the read uses the order it kept, and the next read of the backend reads the flags
permutation s1w s2d s1b s2l s1q s2v s2h s2r s2u s1m s1c s1z s2q s2x
# rows are inserted, and an insert rolled back, while the read waits: it extends the order with them (invisible to its snapshot), the next read sees the committed ones
permutation s1w s1b s2l s1q s2i s2a s2u s1m s1c s1z s2q s2x
