# sift.drop_parts_before against a snapshot that reads the table: a REPEATABLE READ transaction that has read the table still reads every row of it at its next statement; the drop waits for it
# and takes the parts out once it has ended.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_d (id int, at timestamptz NOT NULL, pad text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_d SELECT g, timestamptz '2030-01-01' + g * interval '1 hour', 'x' FROM generate_series(0, 99) g;
}
setup		{ VACUUM (FREEZE) iso_d; }
setup		{ INSERT INTO iso_d SELECT 100 + g, timestamptz '2030-02-01' + g * interval '1 hour', 'x' FROM generate_series(0, 99) g; }
setup		{ VACUUM (FREEZE) iso_d; }

teardown
{
	DROP TABLE iso_d;
}

session r
step rb		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS n FROM iso_d; }
step rn		{ SELECT count(*) AS n FROM iso_d; }
step rend	{ COMMIT; }
step rafter	{ SELECT count(*) AS n FROM iso_d; }

session w
step drop	{ SELECT parts_dropped FROM sift.drop_parts_before('iso_d', timestamptz '2030-01-20'); }

permutation rb drop rn rend rafter
permutation drop rb rn rend rafter
