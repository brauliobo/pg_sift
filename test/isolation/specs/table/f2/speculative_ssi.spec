# Two SERIALIZABLE transactions each read the rows of one group and then add a row to the other with INSERT .. ON CONFLICT DO NOTHING: neither sees the row of the other, which no serial
# order explains. The insert that goes through the arbiter takes the same predicate-lock check as a plain INSERT, so the second to commit is refused.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_ssi (id int PRIMARY KEY, grp int NOT NULL) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_ssi VALUES (1, 1), (2, 2);
}

teardown
{
	DROP TABLE iso_ssi;
}

session s1
step s1begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1read	{ SELECT count(*) AS grp1 FROM iso_ssi WHERE grp = 1; }
step s1add	{ INSERT INTO iso_ssi VALUES (10, 2) ON CONFLICT (id) DO NOTHING; }
step s1c	{ COMMIT; }

session s2
step s2begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2read	{ SELECT count(*) AS grp2 FROM iso_ssi WHERE grp = 2; }
step s2add	{ INSERT INTO iso_ssi VALUES (11, 1) ON CONFLICT (id) DO NOTHING; }
step s2c	{ COMMIT; }

permutation s1begin s2begin s1read s2read s1add s2add s1c s2c
