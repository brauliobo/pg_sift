# VACUUM removing index entries (and inserts reusing the freed line pointers)
# while a bitmap or ranked scan is suspended mid-way in a cursor.
# requires: build insert vacuum scan verify

setup
{
	CREATE TABLE iso_vac (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_vac SELECT g, CASE WHEN g % 3 = 0 THEN 'common rare' ELSE 'common' END || ' text ' || g
		FROM generate_series(1, 3000) g;
	CREATE INDEX iso_vac_idx ON iso_vac USING sift (body);
	DELETE FROM iso_vac WHERE id % 2 = 0;
}

teardown
{
	DROP TABLE iso_vac;
}

session s1
setup		{ CREATE TEMP TABLE IF NOT EXISTS fetched (n serial, id int, score real); TRUNCATE fetched RESTART IDENTITY; SET enable_seqscan = off; }
step s1bitmap	{ BEGIN ISOLATION LEVEL REPEATABLE READ; DECLARE c CURSOR FOR SELECT id FROM iso_vac WHERE body @@> 'common'; }
step s1ranked	{ BEGIN ISOLATION LEVEL REPEATABLE READ; DECLARE c CURSOR FOR SELECT id, sift.score(ctid) AS score FROM iso_vac WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC; }
step s1f	{ SELECT iso_fetch('c', 100) AS fetched; }
step s1rest	{ SELECT iso_fetch('c', 100000) > 0 AS more; }
step s1check	{ SELECT iso_cursor_ok('iso_vac', 'common') AS ok; }
step s1c	{ COMMIT; }

session s2
step s2v	{ VACUUM iso_vac; }
step s2i	{ INSERT INTO iso_vac SELECT g, 'common reused ' || g FROM generate_series(5001, 6500) g; }
step s2d	{ DELETE FROM iso_vac WHERE id % 3 = 0; }
step s2check	{ SELECT iso_agree('iso_vac', 'common') AS agree, iso_agree('iso_vac', 'rare') AS agree_rare, iso_verify('iso_vac_idx') AS verified; }

# entries dead before the scan started are removed and their slots reused mid-scan
permutation s1bitmap s1f s2v s2i s1rest s1check s1c s2check
permutation s1ranked s1f s2v s2i s1rest s1check s1c s2check
# rows deleted after the scan's snapshot stay visible to it through VACUUM
permutation s1ranked s1f s2d s2v s1rest s1check s1c s2v s2check
