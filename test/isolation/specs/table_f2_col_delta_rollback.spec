# A bulk UPDATE of one column of a wide table writes column deltas before it commits. A session that updates another column of a row of it waits for the writer; when the writer rolls back, the row
# is what it was before the writer, with the waiting session's column changed and nothing of the writer's.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_rcd (id int PRIMARY KEY, a int, b int, c int, d int, e int, f int, g int, h int, i int, j int, k text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_rcd SELECT i, i, 0, 0, i, i, i, i, i, i, i, 'row ' || i FROM generate_series(1, 10000) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_rcd');
}

teardown
{
	DROP TABLE iso_rcd;
}

session s1
setup		{ SET deadlock_timeout = '30s'; }
step s1big	{ BEGIN; UPDATE iso_rcd SET b = b + 1 WHERE id <= 6000; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
setup		{ SET deadlock_timeout = '30s'; }
step s2u	{ UPDATE iso_rcd SET c = c + 10 WHERE id = 100; }
step s2v	{ SELECT id, b, c FROM iso_rcd WHERE id IN (100, 7000) ORDER BY id; }
step s2n	{ SELECT count(*) AS n, sum(b) AS sb, sum(c) AS sc FROM iso_rcd; }

permutation s1big s2u s1r s2v s2n
permutation s1big s2u s1c s2v s2n
