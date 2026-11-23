# A transaction that holds more than 4,096 rows of a partition with a shared lock holds the partition (here the relation: a sift_table has one) with one lock of the lock manager, and
# that lock conflicts with what the heap's row locks do: FOR KEY SHARE stops FOR UPDATE alone (a FOREIGN KEY check never blocks a non-key update), FOR SHARE stops FOR NO KEY UPDATE and
# FOR UPDATE, and both let every shared lock through. NOWAIT states each answer without a wait.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_ls (id int PRIMARY KEY, val int, note text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_ls SELECT i, 0, 'row ' || i FROM generate_series(1, 20000) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_ls');
}

teardown
{
	DROP TABLE iso_ls;
}

session s1
step s1ks	{ BEGIN; SELECT count(*) AS locked FROM (SELECT id FROM iso_ls WHERE id <= 6000 FOR KEY SHARE) l; }
step s1sh	{ BEGIN; SELECT count(*) AS locked FROM (SELECT id FROM iso_ls WHERE id <= 6000 FOR SHARE) l; }
step s1nk	{ BEGIN; SELECT count(*) AS locked FROM (SELECT id FROM iso_ls WHERE id <= 6000 FOR NO KEY UPDATE) l; }
step s1c	{ COMMIT; }

session s2
step s2ks	{ SELECT id FROM iso_ls WHERE id = 100 FOR KEY SHARE NOWAIT; }
step s2sh	{ SELECT id FROM iso_ls WHERE id = 100 FOR SHARE NOWAIT; }
step s2nk	{ SELECT id FROM iso_ls WHERE id = 100 FOR NO KEY UPDATE NOWAIT; }
step s2up	{ SELECT id FROM iso_ls WHERE id = 100 FOR UPDATE NOWAIT; }
step s2nonkey	{ UPDATE iso_ls SET val = val + 10 WHERE id = 100; }
step s2v	{ SELECT count(*) AS n, sum(val) AS s FROM iso_ls; }

# held FOR KEY SHARE: a non-key update and FOR NO KEY UPDATE go through, FOR UPDATE is refused
permutation s1ks s2ks s2sh s2nk s2nonkey s2up s1c s2v
# held FOR SHARE: the shared locks go through, FOR NO KEY UPDATE and FOR UPDATE are refused
permutation s1sh s2ks s2sh s2nk s2up s1c s2v
# held FOR NO KEY UPDATE: FOR KEY SHARE goes through, the others are refused
permutation s1nk s2ks s2sh s2nk s2up s1c s2v
