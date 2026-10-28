# The rows a backend buffers before it writes them to the write buffer of a sift_col index (one record a page at the end of the transaction, or when
# the transaction reads the index): a concurrent VACUUM frees the heap slots of the rows of a savepoint that rolled back, or of an ON CONFLICT insert
# that lost, while the transaction goes on and reuses the slots. Those rows must never reach the index, or the index would hold the old address for the
# new row: the pushdown, the index scan and the heap agree at every step. Other sessions never see the rows of the open transaction.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_wb (id int PRIMARY KEY, k int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_wb SELECT g, g % 5, 'x' FROM generate_series(1, 200) g;
	CREATE INDEX iso_wb_idx ON iso_wb USING sift_col (id, k);
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_wb;
}

setup
{
	CREATE FUNCTION iso_wb_q(OUT n bigint, OUT total bigint, OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE q text := 'SELECT count(*), sum(id * (k + 1)) FROM iso_wb WHERE k < 5'; a text; b text; c text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE q INTO n, total;
		a := n || '/' || total;
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		EXECUTE q INTO n, total;
		b := n || '/' || total;
		PERFORM set_config('sift.enable_col_scan', 'off', true);
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
		EXECUTE q INTO n, total;
		c := n || '/' || total;
		agree := a = b AND b = c;
	END $$;
}

teardown
{
	DROP TABLE iso_wb;
	DROP FUNCTION iso_wb_q();
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_wb SELECT 1000 + g, g % 5, 'x' FROM generate_series(1, 60) g; }
step s1sp	{ SAVEPOINT a; }
step s1ia	{ INSERT INTO iso_wb SELECT 2000 + g, g % 5, 'x' FROM generate_series(1, 60) g; }
step s1rb	{ ROLLBACK TO a; }
step s1ib	{ INSERT INTO iso_wb SELECT 3000 + g, g % 5, 'x' FROM generate_series(1, 60) g; }
step s1oc	{ INSERT INTO iso_wb SELECT 100 + g, g % 5, 'x' FROM generate_series(1, 150) g ON CONFLICT (id) DO NOTHING; }
step s1q	{ SELECT n, total, agree FROM iso_wb_q(); }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2v	{ VACUUM iso_wb; }
step s2q	{ SELECT n, total, agree FROM iso_wb_q(); }
step s2x	{ SELECT sift.col_verify('iso_wb_idx')::text = '' AS verified, (SELECT sum(rows) FROM sift.col_segments('iso_wb_idx')) AS index_rows, (SELECT count(*) FROM iso_wb) AS heap_rows; }

# the rows of a savepoint that rolled back: VACUUM frees their slots, the next rows of the transaction take them
permutation s1b s1i s1sp s1ia s1rb s2v s1ib s1q s1c s2q s2x
# ... and when the transaction rolls back as a whole
permutation s1b s1i s1sp s1ia s1rb s2v s1ib s1a s2q s2x
# an ON CONFLICT insert half of whose rows lose: the slots of the lost rows are freed by VACUUM before the transaction ends
permutation s1b s1i s1oc s2v s1ib s1q s1c s2q s2x
# nothing of the open transaction is seen by another session, whatever the transaction has read itself
permutation s1b s1i s1q s2q s1ib s2q s1c s2q s2x
