# A SERIALIZABLE scan of a sift_table reads the pending deletes and the column deltas of a partition again for each row it fetches: a row that another transaction deleted or patched
# after the first look of the scan is a conflict out of the reader. Here the cursor of an index scan fetches a row of the first part, the other transaction reads the mark, deletes
# (or patches) a row of the second part of the same partition and commits, and the cursor fetches that row: the reader comes before the writer, and its write of the mark that the
# writer read makes a cycle of read/write dependencies (40001). The writes are asymmetric, so that only the read of the cursor can name the writer. The maintenance of the setup leaves
# two parts only while no snapshot of the cluster is older than it (else it merges them into one), so the spec runs quiet.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_sc (id int, v int) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_sc_id ON iso_sc (id);
	CREATE TABLE iso_mark (n int);
	INSERT INTO iso_mark VALUES (0);
	INSERT INTO iso_sc SELECT g, 0 FROM generate_series(1, 1000) g;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_sc');
}

setup
{
	INSERT INTO iso_sc SELECT g, 0 FROM generate_series(1001, 2000) g;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_sc');
}

setup
{
	DELETE FROM iso_sc WHERE id = 500;
	UPDATE iso_sc SET v = 1 WHERE id = 501;
	SELECT string_agg(gen || ':' || rows, ' ' ORDER BY gen) AS parts FROM sift.table_segments('iso_sc');
}

teardown
{
	DROP TABLE iso_sc, iso_mark;
}

session s1
setup		{ SET default_transaction_isolation = 'serializable'; SET sift.test_create_format1 = off; SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1warm	{ SELECT count(*), sum(v) FROM iso_sc WHERE id > 0; }
step s1b	{ BEGIN; DECLARE c CURSOR FOR SELECT id, v FROM iso_sc WHERE id IN (1, 2000) ORDER BY id; }
step s1f	{ FETCH 1 FROM c; }
step s1w	{ UPDATE iso_mark SET n = 1; }
step s1c	{ COMMIT; }

session s2
setup		{ SET default_transaction_isolation = 'serializable'; SET sift.test_create_format1 = off; }
step s2b	{ BEGIN; SELECT n FROM iso_mark; }
step s2del	{ DELETE FROM iso_sc WHERE id = 2000; }
step s2upd	{ UPDATE iso_sc SET v = 2 WHERE id = 2000; }
step s2c	{ COMMIT; }

permutation s1warm s1b s1f s2b s2del s2c s1f s1w s1c
permutation s1warm s1b s1f s2b s2upd s2c s1f s1w s1c
