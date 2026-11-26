# An UPDATE of a few columns of a row of a published part is a column delta: the row keeps its address, so a session that waits for the writer of the row finds the same row again
# once it commits. READ COMMITTED re-evaluates the statement's qualification on the row the writer left (EvalPlanQual runs on the latest version, which has the writer's delta and
# the session's own update of the other columns applies over it); REPEATABLE READ fails with a serialization error; a writer that rolls back leaves the row as it was.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_cd (id int PRIMARY KEY, a int, b int, c int, d int, e int, f int, g int, h text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_cd SELECT i, i, 0, 0, i, i, i, i, 'row ' || i FROM generate_series(1, 2000) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_cd');
}

teardown
{
	DROP TABLE iso_cd;
}

session s1
step s1u	{ BEGIN; UPDATE iso_cd SET b = 5 WHERE id = 1; }
step s1i	{ BEGIN; UPDATE iso_cd SET c = c + 1 WHERE id = 1; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2u	{ UPDATE iso_cd SET c = c + 10 WHERE id = 1 AND b <> 5; }
step s2v	{ UPDATE iso_cd SET c = c + 10 WHERE id = 1; }
step s2m	{ UPDATE iso_cd SET c = c + 10 WHERE id = (SELECT id FROM iso_cd WHERE id = 1 FOR UPDATE); }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS n FROM iso_cd WHERE id = 1; }
step s2e	{ ROLLBACK; }
step s2s	{ SELECT concat_ws('/', b, c) AS bc FROM iso_cd WHERE id = 1; }

# the writer commits: the qualification is evaluated again on the row it left (b = 5), does not hold, and nothing is updated
permutation s1u s2u s1c s2s
# the writer rolls back: the row is as it was, the qualification holds and the update applies
permutation s1u s2u s1r s2s
# no qualification on the column the writer changed: both updates are there
permutation s1u s2v s1c s2s
# a snapshot older than the writer's commit cannot update the row
permutation s2rr s1u s1c s2v s2e s2s
# a row mark of the subquery waits for the writer and locks the row it left: the update read the row before the writer's delta, so it meets the delta and runs again on top of it
permutation s1i s2m s1c s2s
