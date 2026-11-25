# SERIALIZABLE write skew through the SiftAgg count of a sift_table (format major 2): the scan reads the parts without an index scan or a heap scan, so the table must take the
# SIREAD lock of the relation itself. Both transactions read the same empty group and insert into it: the second COMMIT fails with 40001.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_t (id int, grp int, bal int) USING sift_table WITH (autovacuum_enabled = off);
	INSERT INTO iso_t SELECT i, 2 + i % 50, i % 100 FROM generate_series(1, 20000) i;
	ANALYZE iso_t;
}

teardown
{
	DROP TABLE iso_t;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1r	{ SELECT count(*) AS n FROM iso_t WHERE grp = 1; }
step s1w	{ INSERT INTO iso_t VALUES (100001, 1, 5); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2r	{ SELECT count(*) AS n FROM iso_t WHERE grp = 1; }
step s2w	{ INSERT INTO iso_t VALUES (200001, 1, 5); }
step s2c	{ COMMIT; }

permutation s1b s2b s1r s2r s1w s2w s1c s2c
