# sift.merge (and VACUUM recycling the merged segments) while a ranked scan
# over those segments is suspended in a cursor.
# requires: build promote merge verify score

setup
{
	CREATE TABLE iso_merge (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_merge SELECT g, 'merge base ' || g FROM generate_series(1, 1000) g;
	CREATE INDEX iso_merge_idx ON iso_merge USING sift (body) WITH (mutable_limit = 64);
	INSERT INTO iso_merge SELECT g, 'merge base second ' || g FROM generate_series(1001, 2000) g;
	SELECT sift.promote('iso_merge_idx');
	INSERT INTO iso_merge SELECT g, 'merge base third base ' || g FROM generate_series(2001, 3000) g;
	SELECT sift.promote('iso_merge_idx');
}

teardown
{
	DROP TABLE iso_merge;
}

session s1
setup		{ CREATE TEMP TABLE IF NOT EXISTS fetched (n serial, id int, score real); TRUNCATE fetched RESTART IDENTITY; }
step s1r	{ BEGIN ISOLATION LEVEL REPEATABLE READ; DECLARE c CURSOR FOR SELECT id, sift.score(ctid) AS score FROM iso_merge WHERE body @@> 'base OR second OR third' ORDER BY sift.score(ctid) DESC; }
step s1f	{ SELECT iso_fetch('c', 50) AS fetched; }
step s1rest	{ SELECT iso_fetch('c', 100000) > 0 AS more; }
step s1check	{ SELECT iso_cursor_ok('iso_merge', 'base OR second OR third') AS ok; }
step s1c	{ COMMIT; }

session s2
step s2m	{ SELECT sift.merge('iso_merge_idx') >= 0 AS merged; }
step s2d	{ DELETE FROM iso_merge WHERE id % 4 = 0; }
step s2v	{ VACUUM iso_merge; }
step s2check	{ SELECT iso_agree('iso_merge', 'base OR second OR third') AS agree, iso_agree('iso_merge', 'third NOT second') AS agree_not, iso_verify('iso_merge_idx') AS verified; }

permutation s1r s1f s2m s1rest s1check s1c s2check
# deleted rows stay visible to the older snapshot across merge and VACUUM
permutation s1r s1f s2d s2v s2m s2v s1rest s1check s1c s2v s2check
