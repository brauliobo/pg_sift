# A chain of column deltas long enough to have a directory (SIFT_PT_COL_DELTA_DIR) is read by the snapshot of each session: a repeatable-read session keeps its rows while a bulk UPDATE of the
# chain commits or rolls back, and the session that comes after it sees the rows of every committed writer and none of a rolled back one.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_cdd (id int PRIMARY KEY, a int, b int, c int, d int, e int, f int, g int, h int, k text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_cdd SELECT i, i, 0, 0, i, i, i, i, i, 'row ' || i FROM generate_series(1, 20000) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_cdd');
	UPDATE iso_cdd SET c = c + 1 WHERE id <= 20000;
}

teardown
{
	DROP TABLE iso_cdd;
}

session s1
step s1big	{ BEGIN; UPDATE iso_cdd SET b = b + 1 WHERE id <= 20000; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS n, sum(b) AS sb, sum(c) AS sc FROM iso_cdd; }
step s2r	{ SELECT id, b, c FROM iso_cdd WHERE id IN (1, 10000, 20000) ORDER BY id; SELECT count(*) AS n, sum(b) AS sb, sum(c) AS sc FROM iso_cdd; }
step s2e	{ COMMIT; }

session s3
step s3r	{ SELECT id, b, c FROM iso_cdd WHERE id IN (1, 10000, 20000) ORDER BY id; SELECT count(*) AS n, sum(b) AS sb, sum(c) AS sc FROM iso_cdd; }

permutation s2b s1big s2r s1c s2r s2e s3r
permutation s2b s1big s2r s1r s2r s2e s3r
