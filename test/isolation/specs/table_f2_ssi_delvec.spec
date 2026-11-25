# Two SERIALIZABLE transactions of a sift_table that each read the row the other one deletes are a cycle of read/write dependencies: one of them fails with 40001. The delete of a transaction
# that committed after the snapshot of the other is in the deletion vector of the part, kept with its deleter until no snapshot is older than the commit; a read that skips the rows of the vector
# without asking who deleted them never learns of the dependency on the deleter.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_ssi (id int PRIMARY KEY, grp int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_ssi VALUES (1, 1), (2, 2);
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_ssi');
}

teardown
{
	DROP TABLE iso_ssi;
}

session s1
setup		{ SET default_transaction_isolation = 'serializable'; }
step s1b	{ BEGIN; }
step s1rd	{ SELECT id FROM iso_ssi WHERE grp = 2; }
step s1del	{ DELETE FROM iso_ssi WHERE id = 1; }
step s1c	{ COMMIT; }

session s2
setup		{ SET default_transaction_isolation = 'serializable'; }
step s2b	{ BEGIN; SELECT 1; }
step s2rd	{ SELECT id FROM iso_ssi WHERE grp = 1; }
step s2del	{ DELETE FROM iso_ssi WHERE id = 2; }
step s2c	{ COMMIT; }

session s3
step s3m	{ SELECT count(*) AS busy FROM sift.table_maintain('iso_ssi'); }

permutation s2b s1b s1rd s1del s1c s3m s2rd s2del s2c
