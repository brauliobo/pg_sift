# The fetch of a transaction by address keeps what it learned of the transactions of one snapshot, and keeps it only for that snapshot: a READ COMMITTED statement takes a new snapshot, which may
# live at the address of the one the statement before it freed, and a prepared transaction that commits changes no part of the table, so nothing else tells the fetch that what it learned is old. The
# row the prepared transaction deleted is gone for the second statement.
# Needs max_prepared_transactions > 0 (scripts/pgtest.sh sets it).
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_aba (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_aba VALUES (1, 10), (2, 20);
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_aba');
	CREATE TABLE iso_aba_addr AS SELECT ctid AS addr FROM iso_aba WHERE id = 1;
}

teardown
{
	DROP TABLE iso_aba, iso_aba_addr;
}

session s1
step s1del	{ BEGIN; DELETE FROM iso_aba WHERE id = 1; PREPARE TRANSACTION 'sift_aba'; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2at	{ SELECT id FROM iso_aba WHERE ctid = (SELECT addr FROM iso_aba_addr); }
step s2c	{ COMMIT; }

session s3
step s3cp	{ COMMIT PREPARED 'sift_aba'; }

permutation s1del s2b s2at s3cp s2at s2c
