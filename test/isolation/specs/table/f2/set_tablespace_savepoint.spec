# The row locks of a transaction survive a SET TABLESPACE of the table that it rolls back to a savepoint: a row it locked FOR UPDATE and a row it deleted before the savepoint stay
# locked once the table is back on its file, and a writer of another session waits for the transaction and then sees its delete.
# requires: tam

setup		{ SET allow_in_place_tablespaces = on; }
setup		{ CREATE TABLESPACE iso_stsp_ts LOCATION ''; }
setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	CREATE TABLE iso_stsp (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_stsp SELECT i, 0 FROM generate_series(1, 10) i;
}

teardown
{
	DROP TABLE iso_stsp;
}

session s1
step s1lock		{ BEGIN; SELECT id FROM iso_stsp WHERE id = 1 FOR UPDATE; DELETE FROM iso_stsp WHERE id = 2; }
step s1move		{ SAVEPOINT s; ALTER TABLE iso_stsp SET TABLESPACE iso_stsp_ts; ROLLBACK TO s; }
step s1commit	{ COMMIT; }
teardown		{ DROP TABLESPACE iso_stsp_ts; }

session s2
step s2upd1		{ UPDATE iso_stsp SET val = val + 1 WHERE id = 1; }
step s2upd2		{ UPDATE iso_stsp SET val = val + 1 WHERE id = 2; }
step s2read		{ SELECT id, val FROM iso_stsp WHERE id <= 2 ORDER BY id; }

permutation s1lock s1move s2upd1 s1commit s2read
permutation s1lock s1move s2upd2 s1commit s2read
