# A chain of column deltas too long for the cache of the chains is read for the records of the writers not past every snapshot of the backend. A REPEATABLE READ transaction that took its
# snapshot before another patched a row is refused when it patches the same row, even after a catalog snapshot newer than the patch (DDL of another session) moved RecentXmin past its writer;
# and a MERGE that meets a row its own command patched on that chain is refused as the heap refuses it.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_lf (id int PRIMARY KEY, b int NOT NULL, c int NOT NULL) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_lf SELECT i, 0, 0 FROM generate_series(1, 4000) i;
}

setup
{
	UPDATE iso_lf SET b = b + 1;
	UPDATE iso_lf SET b = b + 1;
}

teardown
{
	DROP TABLE iso_lf;
}

session s1
setup		{ SET sift.backend_cache_kb = '1MB'; }
step s1upd	{ UPDATE iso_lf SET c = c + 1 WHERE id = 5; }
step s1ddl	{ CREATE TABLE iso_lf_ddl (a int); DROP TABLE iso_lf_ddl; }

session s2
setup		{ SET sift.backend_cache_kb = '1MB'; }
step s2begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS rows FROM iso_lf; }
step s2other	{ UPDATE iso_lf SET c = c + 10 WHERE id = 6; }
step s2cat	{ SELECT to_regclass('iso_lf_none') IS NULL AS none; }
step s2same	{ UPDATE iso_lf SET c = c + 10 WHERE id = 5; }
step s2c	{ COMMIT; }
step s2merge	{ MERGE INTO iso_lf USING (SELECT 7 AS id UNION ALL SELECT 7) s ON iso_lf.id = s.id WHEN MATCHED THEN UPDATE SET c = c + 1; }
step s2mergeall	{ MERGE INTO iso_lf USING (SELECT id FROM generate_series(1, 4000) id UNION ALL SELECT 4000) s ON iso_lf.id = s.id WHEN MATCHED THEN UPDATE SET c = c + 1; }
step s2v	{ SELECT id, c FROM iso_lf WHERE id IN (5, 6, 7, 4000) ORDER BY id; }

# the log is read before the patch, and RecentXmin moves before the chain is read again
permutation s2begin s2other s1upd s1ddl s2cat s2same s2c s2v
# the log is first read after RecentXmin moved
permutation s2begin s1upd s1ddl s2cat s2same s2c s2v
# a row the command patched twice: a few rows, and a command whose row locks escalate
permutation s2merge s2mergeall s2v
