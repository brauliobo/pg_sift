# Inserts racing the seal of the open write buffer and its promotion,
# with the inserting transaction committed or rolled back afterwards.
# requires: build insert scan promote verify

setup
{
	CREATE TABLE iso_seal (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_seal SELECT g, 'seed doc ' || g FROM generate_series(1, 200) g;
	CREATE INDEX iso_seal_idx ON iso_seal USING sift (body) WITH (write_buffer_limit = 64);
}

teardown
{
	DROP TABLE iso_seal;
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_seal SELECT g, 'alpha one ' || g || repeat(' filler', 20) FROM generate_series(1000, 1999) g; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2i	{ INSERT INTO iso_seal SELECT g, 'alpha two ' || g || repeat(' filler', 20) FROM generate_series(2000, 2999) g; }
step s2p	{ SELECT sift.promote('iso_seal_idx') >= 0 AS promoted; }

session s3
step s3q	{ SELECT iso_agree('iso_seal', 'alpha') AS agree, iso_agree('iso_seal', 'one OR seed') AS agree_or, iso_verify('iso_seal_idx') AS verified; }

# concurrent appends across seals, promotion while one inserter is in flight
permutation s1b s1i s2i s2p s1c s3q
# promotion captures records of a transaction that then aborts
permutation s1b s1i s2p s2i s1a s3q
# readers between promotion and commit see only committed rows
permutation s1b s2i s1i s2p s3q s1c s3q
