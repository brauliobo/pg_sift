# The automatic TTL (the pass of VACUUM) against a snapshot that reads the table: a REPEATABLE READ or SERIALIZABLE transaction that has read the table still reads every row of it at its next
# statement, so the pass leaves the expired part where it is and takes it out in the pass after the transaction has ended; it never waits for the reader.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_t (id int, at timestamptz NOT NULL, pad text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_t SELECT g, now() - interval '30 days' + g * interval '1 second', 'x' FROM generate_series(0, 99) g;
}
setup		{ VACUUM (FREEZE) iso_t; }
setup		{ INSERT INTO iso_t SELECT 100 + g, now() + g * interval '1 second', 'x' FROM generate_series(0, 99) g; }
setup		{ VACUUM (FREEZE) iso_t; }
setup		{ SELECT sift.set_ttl('iso_t', 'at', interval '1 day'); }

teardown
{
	DROP TABLE iso_t;
}

session r
step rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS n FROM iso_t; }
step rs		{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) AS n FROM iso_t; }
step rn		{ SELECT count(*) AS n FROM iso_t; }
step rend	{ COMMIT; }
step rafter	{ SELECT count(*) AS n FROM iso_t; }

session w
step tick	{ VACUUM iso_t; }
step tick2	{ VACUUM iso_t; }

permutation rr tick rn rend tick2 rafter
permutation rs tick rn rend tick2 rafter
permutation tick rr rn rend rafter
