# SERIALIZABLE on a table with a sift index: two transactions that each search for a key (no row has it) and then write are a write skew, and the second one to
# write fails with 40001, whether the search ran as a bitmap scan (sift.enable_scan off: the plan of a sift index that has no custom scan to take its place), as the
# count or as the ranked scan, and whether the other writes an insert or a delete.
# requires: build scan score

setup
{
	CREATE TABLE iso_ssi (id int, body text);
	INSERT INTO iso_ssi SELECT g, 'common word ' || g FROM generate_series(1, 20) g;
	CREATE INDEX iso_ssi_idx ON iso_ssi USING sift (body);
}

teardown
{
	DROP TABLE iso_ssi;
}

session s1
setup		{ SET enable_seqscan = off; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1bm	{ SET LOCAL sift.enable_scan = off; SELECT id FROM iso_ssi WHERE body @@> 'unicorn'; }
step s1bc	{ SET LOCAL sift.enable_scan = off; SELECT count(*) AS found FROM iso_ssi WHERE body @@> 'common word 1'; }
step s1ct	{ SELECT count(*) AS found FROM iso_ssi WHERE body @@> 'unicorn'; }
step s1rk	{ SELECT id FROM iso_ssi WHERE body @@> 'unicorn' ORDER BY sift.score(ctid) DESC LIMIT 3; }
step s1i	{ INSERT INTO iso_ssi VALUES (101, 'unicorn'); }
step s1d	{ DELETE FROM iso_ssi WHERE id = 1; }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; }
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2bm	{ SET LOCAL sift.enable_scan = off; SELECT id FROM iso_ssi WHERE body @@> 'unicorn'; }
step s2bc	{ SET LOCAL sift.enable_scan = off; SELECT count(*) AS found FROM iso_ssi WHERE body @@> 'common word 1'; }
step s2ct	{ SELECT count(*) AS found FROM iso_ssi WHERE body @@> 'unicorn'; }
step s2rk	{ SELECT id FROM iso_ssi WHERE body @@> 'unicorn' ORDER BY sift.score(ctid) DESC LIMIT 3; }
step s2i	{ INSERT INTO iso_ssi VALUES (102, 'unicorn'); }
step s2c	{ COMMIT; }

# each looks for the key through the bitmap scan, then inserts it
permutation s1b s2b s1bm s2bm s1i s1c s2i s2c
# the same through the count
permutation s1b s2b s1ct s2ct s1i s1c s2i s2c
# the same through the ranked scan
permutation s1b s2b s1rk s2rk s1i s1c s2i s2c
# both read the rows of a term, one deletes one of them, the other inserts
permutation s1b s2b s1bc s2bc s1d s1c s2i s2c
