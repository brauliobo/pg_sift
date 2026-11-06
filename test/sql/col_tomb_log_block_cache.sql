-- A scan keeps what the tombstone log says of a heap block (scan_unit.c): the units of a key-cut segment list the same pages again and again, and the second ask for a block is
-- answered from the scan, not from the log. The answer must be the log's: every case runs the same aggregate through the index and through the heap (the oracle), over rows
-- updated and deleted since the last VACUUM, and says whether asks were served from what the scan kept. A mutant that keeps the answer of one block for another, or of an older
-- snapshot, must give the oracle's rows no more.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE bc (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO bc SELECT i, i % 40, i, 'x' FROM generate_series(1, 40000) i;
CREATE INDEX bc_c ON bc USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id', key_cut = tenant_id);
VACUUM bc;

-- dynamic SQL: a cached plan of a SQL function would keep the plan of the first call whatever sift.enable_agg says
CREATE FUNCTION bc_answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', tenant_id, n, s), '' '' ORDER BY tenant_id), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;

-- one row per case: the index gives the heap's rows for the whole table, a set of keys and one key, and the asks the scans answered from what they kept
CREATE FUNCTION bc_chk(what text, OUT test text, OUT same_all bool, OUT same_set bool, OUT same_one bool, OUT served_from_scan bool) LANGUAGE plpgsql AS $$
DECLARE
	qa text := 'SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM bc GROUP BY tenant_id';
	qs text := 'SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM bc WHERE tenant_id BETWEEN 5 AND 14 GROUP BY tenant_id';
	qo text := 'SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM bc WHERE tenant_id = 7 GROUP BY tenant_id';
	ia text; is_ text; io text; hits bigint;
BEGIN
	PERFORM sift._test_hf_block_hits(true);
	PERFORM set_config('sift.enable_agg', 'on', true);
	ia := bc_answer(qa); is_ := bc_answer(qs); io := bc_answer(qo);
	hits := sift._test_hf_block_hits(true);
	PERFORM set_config('sift.enable_agg', 'off', true);
	test := what;
	same_all := ia = bc_answer(qa);
	same_set := is_ = bc_answer(qs);
	same_one := io = bc_answer(qo);
	served_from_scan := hits > 0;
END $$;

SELECT * FROM bc_chk('fresh build');

-- an update of a covered column: new entries in the write buffer, a tombstone for each old row; the pages of the heap are no longer all-visible
UPDATE bc SET counter_a = counter_a + 5 WHERE id % 11 = 0;
SELECT * FROM bc_chk('update, rows in the buffer');
SELECT sift.col_promote('bc_c') >= 0 AS promoted;
SELECT * FROM bc_chk('update, promoted');

-- deletes, and a change of the key (the row moves to another unit)
DELETE FROM bc WHERE id % 13 = 0;
SELECT * FROM bc_chk('delete');
UPDATE bc SET tenant_id = (tenant_id + 1) % 40 WHERE id % 17 = 0;
SELECT * FROM bc_chk('key change');
SELECT sift.col_promote('bc_c') >= 0 AS promoted;
SELECT * FROM bc_chk('key change, promoted');

-- a transaction that did not commit leaves tombstones nobody sees
BEGIN; DELETE FROM bc WHERE id % 23 = 0; ROLLBACK;
SELECT * FROM bc_chk('aborted delete');

-- a transaction that sees its own changes: the log holds its entries, which are its own, and the scan keeps what it says for its snapshot
BEGIN;
UPDATE bc SET counter_a = counter_a + 1 WHERE id % 29 = 0;
SELECT * FROM bc_chk('own update');
DELETE FROM bc WHERE id % 31 = 0;
SELECT * FROM bc_chk('own delete');
ROLLBACK;

VACUUM bc;
SELECT * FROM bc_chk('after VACUUM');

DROP FUNCTION bc_chk(text), bc_answer(text);
DROP TABLE bc;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
