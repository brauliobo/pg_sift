# sift.approx_percentile checks the right to read the table before it locks it: a user who may not read ka is refused at once, without waiting for the lock another session holds.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE ka (id int8 PRIMARY KEY, v float8) WITH (autovacuum_enabled = off);
	INSERT INTO ka SELECT i, i FROM generate_series(1, 1000) i;
	CREATE INDEX ka_col ON ka USING sift_col (id, v) WITH (sketches = 'kll(v)');
	CREATE ROLE regress_kll_reader;
}

teardown
{
	DROP TABLE ka;
	DROP ROLE regress_kll_reader;
}

session s1
step s1lock	{ BEGIN; LOCK TABLE ka IN ACCESS EXCLUSIVE MODE; }
step s1c	{ COMMIT; }

session s2
setup		{ SET ROLE regress_kll_reader; }
step s2r	{ SELECT sift.approx_percentile('ka', 'v', 0.5) AS p; }

permutation s1lock s2r s1c
