# A bulk UPDATE of one column of a wide table is column deltas and holds the relation with one lock of the lock manager. A session that updates another column of a row of it waits for
# the writer, runs EvalPlanQual on the row the writer left, and applies its own column over the writer's: the row has both changes, and the statement does not start over for ever.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_lcd (id int PRIMARY KEY, a int, b int, c int, d int, e int, f int, g int, h int, i int, j int, k text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_lcd SELECT i, i, 0, 0, i, i, i, i, i, i, i, 'row ' || i FROM generate_series(1, 50000) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_lcd');
}

teardown
{
	DROP TABLE iso_lcd;
}

session s1
setup		{ SET deadlock_timeout = '30s'; }
step s1big	{ BEGIN; UPDATE iso_lcd SET b = b + 1 WHERE id <= 34000; }
step s1c	{ COMMIT; }

session s2
setup		{ SET deadlock_timeout = '30s'; }
step s2u	{ UPDATE iso_lcd SET c = c + 10 WHERE id = 100; }
step s2q	{ UPDATE iso_lcd SET c = c + 10 WHERE id = 100 AND b = 0; }
step s2ks	{ SELECT id FROM iso_lcd WHERE id = 100 FOR KEY SHARE; }
step s2v	{ SELECT id, b, c FROM iso_lcd WHERE id IN (100, 35000) ORDER BY id; }
step s2n	{ SELECT count(*) AS n, sum(b) AS sb, sum(c) AS sc FROM iso_lcd; }

# the other column applies over the writer's
permutation s1big s2u s1c s2v s2n
# the qualification reads the column the writer changed: after the commit it no longer holds
permutation s1big s2q s1c s2v s2n
# a KEY SHARE lock of a row the bulk update changed is granted at once
permutation s1big s2ks s1c s2n
