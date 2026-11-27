# sift.approx_percentile locks the table before its index, as DDL on the table does: a quantile of the index that waits for a lock of the table holds none of the index, so a TRUNCATE
# or a DROP INDEX in the transaction that holds the table goes on (no deadlock), and the quantile is the sketch's of the emptied table or, the index gone, the exact one of the table.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE kl (id int8 PRIMARY KEY, v float8) WITH (autovacuum_enabled = off);
	INSERT INTO kl SELECT i, i FROM generate_series(1, 1000) i;
	CREATE INDEX kl_col ON kl USING sift_col (id, v) WITH (sketches = 'kll(v)');
}

teardown
{
	DROP TABLE kl;
}

session s1
step s1lock		{ BEGIN; LOCK TABLE kl IN ACCESS EXCLUSIVE MODE; }
step s1trunc	{ TRUNCATE kl; }
step s1drop		{ DROP INDEX kl_col; }
step s1c		{ COMMIT; }

session s2
step s2q		{ SELECT sift.approx_percentile('kl_col', 'v', 0.5) AS p; }

permutation s1lock s2q s1trunc s1c
permutation s1lock s2q s1drop s1c
