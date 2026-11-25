# A statement that updates more than 32,768 rows of a partition holds the partition (here the relation: a sift_table has one) with one lock of the lock manager instead of one entry for
# each row. What the entries gave stays: a KEY SHARE lock, which a non-key update does not conflict with, is granted; NOWAIT is refused and SKIP LOCKED skips; READ COMMITTED
# runs EvalPlanQual on the row the writer left; REPEATABLE READ fails with 40001; two transactions that both go from rows to the partition at once are a deadlock that the
# lock manager reports to one of them. A plain read takes no lock of the table and never waits.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_le (id int PRIMARY KEY, val int, note text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE TABLE iso_other (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_le SELECT i, 0, 'row ' || i FROM generate_series(1, 80000) i;
	INSERT INTO iso_other SELECT i, 0 FROM generate_series(1, 100) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_le');
}

teardown
{
	DROP TABLE iso_le, iso_other;
}

session s1
setup		{ SET deadlock_timeout = '30s'; }
step s1big	{ BEGIN; UPDATE iso_le SET val = val + 1 WHERE id <= 34000; }
step s1a	{ BEGIN; UPDATE iso_le SET val = val + 1 WHERE id <= 18000; }
step s1b	{ UPDATE iso_le SET val = val + 1 WHERE id BETWEEN 18001 AND 34000; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
setup		{ SET deadlock_timeout = '50ms'; }
step s2other	{ UPDATE iso_other SET val = val + 1 WHERE id = 1; }
step s2read	{ SELECT count(*) AS n, sum(val) AS s FROM iso_le WHERE id <= 34000; }
step s2ks	{ BEGIN; SELECT id FROM iso_le WHERE id = 100 FOR KEY SHARE; }
step s2nowait	{ SELECT id FROM iso_le WHERE id = 100 FOR UPDATE NOWAIT; }
step s2skip	{ SELECT id FROM iso_le WHERE id IN (100, 35000) ORDER BY id FOR UPDATE SKIP LOCKED; }
step s2gone	{ UPDATE iso_le SET val = val + 10 WHERE id = 100 AND val = 0; }
step s2kept	{ UPDATE iso_le SET val = val + 10 WHERE id = 100 AND val >= 0; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS n FROM iso_le WHERE id = 100; }
step s2rrw	{ UPDATE iso_le SET val = 0 WHERE id = 100; }
step s2rru	{ UPDATE iso_le SET val = val + 1 WHERE id = 35000; }
step s2c	{ COMMIT; }
step s2e	{ ROLLBACK; }
step s2a	{ BEGIN; UPDATE iso_le SET val = val + 1 WHERE id BETWEEN 36001 AND 54000; }
step s2b	{ UPDATE iso_le SET val = val + 1 WHERE id BETWEEN 54001 AND 72000; }
step s2v	{ SELECT count(*) AS n, sum(val) AS s FROM iso_le; }

# other tables and plain reads do not wait for the partition; a KEY SHARE lock is granted
permutation s1big s2other s2read s2ks s2c s1c s2v
# NOWAIT is refused at once, SKIP LOCKED skips the row of the writer and takes the other
permutation s1big s2nowait s2skip s1r s2v
# READ COMMITTED: the qualification is evaluated again on the row the writer left
permutation s1big s2gone s1c s2v
permutation s1big s2kept s1c s2v
# the writer rolls back: the row is as it was and the update applies
permutation s1big s2kept s1r s2v
# REPEATABLE READ: a row the writer changed is a serialization failure, a row it did not touch is an update once it has committed
permutation s2rr s1big s1c s2rrw s2e s2v
permutation s2rr s1big s1c s2rru s2e s2v
# two transactions that go from rows to the partition at once: the lock manager reports the deadlock to one of them
permutation s1a s2a s1b s2b s1c s2e s2v
