# A savepoint that rolled back gives back the strongest lock it took: the partition (here the relation: a sift_table has one) that the outer transaction holds when it locks enough rows for
# the escalation is held in the mode of the rows the transaction still holds, a FOR SHARE one, and does not stop another FOR SHARE of the table.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_hm (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_hm SELECT i, 0 FROM generate_series(1, 20000) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_hm');
}

teardown
{
	DROP TABLE iso_hm;
}

session s1
setup		{ SET deadlock_timeout = '30s'; }
step s1begin	{ BEGIN; }
step s1sp	{ SAVEPOINT a; }
step s1ex	{ SELECT id FROM iso_hm WHERE id = 1 FOR UPDATE; }
step s1rb	{ ROLLBACK TO a; }
step s1share	{ SELECT count(*) AS n FROM (SELECT id FROM iso_hm WHERE id BETWEEN 10 AND 5009 FOR SHARE) q; }
step s1c	{ COMMIT; }

session s2
setup		{ SET deadlock_timeout = '30s'; }
step s2share	{ SELECT id FROM iso_hm WHERE id = 9000 FOR SHARE; }

permutation s1begin s1sp s1ex s1rb s1share s2share s1c
