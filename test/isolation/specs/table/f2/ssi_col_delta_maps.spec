# A SERIALIZABLE index or bitmap scan of a sift_table keeps the column-delta map it read of a part for the next rows it fetches of that part: a row another transaction patched
# since that read is a conflict out of the reader all the same. The cursor fetches a row, the other transaction reads the mark, patches a later row of the same part and commits, and
# the cursor fetches that row: the reader comes before the writer, and its write of the mark the writer read makes a cycle of read/write dependencies (40001). The rows are in other
# blocks of their granule, so that the page lock of the first row does not cover the patched one. Permutations: the part had no column delta before (its entry has no flag), the
# map of one granule, the map of the whole part (a third granule), and the inner bitmap scan of a nested loop that runs again for the next key.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_cdm (id int, v int) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_cdm_id ON iso_cdm (id);
	CREATE TABLE iso_cdm_flag (id int, v int) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_cdm_flag_id ON iso_cdm_flag (id);
	CREATE TABLE iso_mark (n int);
	INSERT INTO iso_mark VALUES (0);
	INSERT INTO iso_cdm SELECT g, 0 FROM generate_series(1, 30000) g;
	INSERT INTO iso_cdm_flag SELECT g, 0 FROM generate_series(1, 30000) g;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_cdm');
	SELECT busy FROM sift.table_maintain('iso_cdm_flag');
}

setup
{
	UPDATE iso_cdm SET v = 1 WHERE id = 29000;
}

teardown
{
	DROP TABLE iso_cdm, iso_cdm_flag, iso_mark;
}

session s1
setup		{ SET default_transaction_isolation = 'serializable'; SET sift.enable_agg = off; SET enable_seqscan = off; SET max_parallel_workers_per_gather = 0; }
step s1flag	{ BEGIN; SET LOCAL enable_bitmapscan = off; DECLARE c CURSOR FOR SELECT id, v FROM iso_cdm_flag WHERE id IN (1, 2000) ORDER BY id; }
step s1one	{ BEGIN; SET LOCAL enable_bitmapscan = off; DECLARE c CURSOR FOR SELECT id, v FROM iso_cdm WHERE id IN (1, 2000) ORDER BY id; }
step s1whole	{ BEGIN; SET LOCAL enable_bitmapscan = off; DECLARE c CURSOR FOR SELECT id, v FROM iso_cdm WHERE id IN (1, 9000, 17000) ORDER BY id; }
step s1loop	{ BEGIN; SET LOCAL enable_indexscan = off; SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off; SET LOCAL enable_material = off; SET LOCAL enable_memoize = off;
			  DECLARE c CURSOR FOR SELECT o.k, t.v FROM (VALUES (1), (2000)) o(k) JOIN iso_cdm t ON t.id = o.k; }
step s1f	{ FETCH 1 FROM c; }
step s1w	{ UPDATE iso_mark SET n = 1; }
step s1c	{ COMMIT; }

session s2
setup		{ SET default_transaction_isolation = 'serializable'; }
step s2b	{ BEGIN; SELECT n FROM iso_mark; }
step s2flag	{ UPDATE iso_cdm_flag SET v = 2 WHERE id = 2000; }
step s2one	{ UPDATE iso_cdm SET v = 2 WHERE id = 2000; }
step s2whole	{ UPDATE iso_cdm SET v = 2 WHERE id = 17000; }
step s2c	{ COMMIT; }

permutation s1flag s1f s2b s2flag s2c s1f s1w s1c
permutation s1one s1f s2b s2one s2c s1f s1w s1c
permutation s1whole s1f s1f s2b s2whole s2c s1f s1w s1c
permutation s1loop s1f s2b s2one s2c s1f s1w s1c
