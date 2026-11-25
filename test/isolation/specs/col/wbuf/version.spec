# A column scan keeps the batches of the write buffer a backend read before, as long as the directory version is the one it read when it opened (the
# version VACUUM bumps after it flags records, before the heap frees their addresses). The scan opens, holds on the test gate while VACUUM flags the
# records of rows that were deleted before its snapshot, the heap frees their addresses and HOT updates reuse them, and then reads the batches it kept:
# the answer is that of the heap at the snapshot, in the aggregate pushdown and in the index scan, and the next scan reads the flags.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_wv (id int PRIMARY KEY, k int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_wv SELECT g, g % 5, 'x' FROM generate_series(1, 200) g;
	CREATE INDEX iso_wv_idx ON iso_wv USING sift_col (id, k);
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_wv;
}

# the rows of the write buffer, on the last page of the heap; the addresses of the last 30 are noted
setup
{
	INSERT INTO iso_wv SELECT 1000 + g, g % 5, 'x' FROM generate_series(1, 60) g;
	CREATE TABLE iso_wv_gone AS SELECT ctid AS t FROM iso_wv WHERE id > 1030;
}

setup
{
	CREATE FUNCTION iso_wv_q(path text, gate int, OUT n bigint, OUT total bigint, OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE q text := 'SELECT count(*), sum(id * (k + 1)) FROM iso_wv WHERE k < 5'; a text; b text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
		PERFORM set_config('enable_indexscan', (path <> 'agg')::text, true);
		PERFORM set_config('enable_indexonlyscan', (path <> 'agg')::text, true);
		IF path = 'agg' THEN
			PERFORM set_config('sift.enable_agg', 'on', true);
		ELSE
			PERFORM set_config('sift.enable_agg', 'off', true);
			q := 'SELECT count(*), sum(id * (k + 1)) FROM (SELECT id, k FROM iso_wv WHERE k < 5 OFFSET 0) s';
		END IF;
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		EXECUTE q INTO n, total;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		a := n || '/' || total;
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'off', true);
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
		EXECUTE q INTO n, total;
		b := n || '/' || total;
		agree := a = b;
	END $$;
}

teardown
{
	DROP TABLE iso_wv, iso_wv_gone;
	DROP FUNCTION iso_wv_q(text, int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_wv; }
step s1wa	{ SELECT n, total, agree FROM iso_wv_q('agg', 0); }
step s1wi	{ SELECT n, total, agree FROM iso_wv_q('scan', 0); }
step s1qa	{ SELECT n, total, agree FROM iso_wv_q('agg', 7001); }
step s1qi	{ SELECT n, total, agree FROM iso_wv_q('scan', 7001); }
step s1c	{ COMMIT; }
step s1z	{ SELECT n, total, agree FROM iso_wv_q('agg', 0); }
step s1y	{ SELECT n, total, agree FROM iso_wv_q('scan', 0); }

session s2
step s2d	{ DELETE FROM iso_wv WHERE id > 1030; }
step s2l	{ SELECT pg_advisory_lock(7001); }
step s2v	{ VACUUM iso_wv; }
step s2h	{ UPDATE iso_wv SET pad = 'y' WHERE id BETWEEN 1001 AND 1030; }
step s2r	{ SELECT count(*) AS reused FROM iso_wv w JOIN iso_wv_gone g ON w.ctid = g.t; }
step s2u	{ SELECT pg_advisory_unlock(7001); }
step s2q	{ SELECT n, total, agree FROM iso_wv_q('agg', 0); }
step s2x	{ SELECT sift.col_verify('iso_wv_idx')::text = '' AS verified, (SELECT sum(rows) FROM sift.col_segments('iso_wv_idx')) AS index_rows, (SELECT count(*) FROM iso_wv) AS heap_rows; }

# the backend keeps the batches; rows are deleted; a snapshot is taken, the aggregate opens, VACUUM flags the records of the deleted rows, the heap frees their
# addresses and the HOT updates of the rows next to them reuse them (a heap-only version has no record of its own, so a record left at its address would
# be taken for a row of its own); the aggregate reads the batches it kept, and the next aggregate of the backend reads the flags
permutation s1wa s2d s1b s2l s1qa s2v s2h s2r s2u s1c s1z s2q s2x
# ... and the index scan
permutation s1wi s2d s1b s2l s1qi s2v s2h s2r s2u s1c s1y s2q s2x
