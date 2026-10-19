# Facet counts while rows change: a snapshot's counts are the GROUP BY of the same snapshot, whether the
# writes are uncommitted, deleted and vacuumed, updated, merged away or reuse the freed line pointers.
# requires: build insert vacuum scan promote merge verify facets

setup
{
	CREATE TABLE iso_fac (id int PRIMARY KEY, body text, cat text, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_fac SELECT g, 'common w' || g % 5, 'c' || g % 7, repeat('p', 1000) FROM generate_series(1, 3000) g;
	CREATE INDEX iso_fac_body ON iso_fac USING sift (body) WITH (write_buffer_limit = 64);
	CREATE INDEX iso_fac_cat ON iso_fac USING sift (cat) WITH (analyzer = keyword, write_buffer_limit = 64);
	CREATE FUNCTION iso_facets_ok(q text) RETURNS bool LANGUAGE plpgsql
		SET sift.enable_scan = off SET sift.enable_count = off SET enable_bitmapscan = off SET enable_indexscan = off AS $$
	DECLARE
		got text;
		want text;
	BEGIN
		SELECT string_agg(value || '=' || count, ' ' ORDER BY ord) INTO got
			FROM sift.facets('iso_fac_body', 'iso_fac_cat', q, NULL) WITH ORDINALITY f(value, count, ord);
		SELECT string_agg(cat || '=' || n, ' ' ORDER BY n DESC, cat COLLATE "C") INTO want
			FROM (SELECT cat, count(*) n FROM iso_fac WHERE body @@> q AND cat IS NOT NULL GROUP BY cat) s;
		RETURN got IS NOT DISTINCT FROM want;
	END $$;
}

teardown
{
	DROP TABLE iso_fac;
	DROP FUNCTION iso_facets_ok(text);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS rows FROM iso_fac; }
step s1check	{ SELECT iso_facets_ok('common') AS ok_common, iso_facets_ok('w1 OR w2') AS ok_or, iso_facets_ok('w3 AND NOT w4') AS ok_not; }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2c	{ COMMIT; }
step s2a	{ ROLLBACK; }
step s2i	{ INSERT INTO iso_fac SELECT g, 'common w' || g % 5, 'n' || g % 3, repeat('p', 1000) FROM generate_series(4000, 4999) g; }
step s2d	{ DELETE FROM iso_fac WHERE id % 3 = 0; }
step s2u	{ UPDATE iso_fac SET cat = 'moved' WHERE id % 4 = 0; }
step s2v	{ VACUUM iso_fac; }
step s2r	{ INSERT INTO iso_fac SELECT g, 'common w' || g % 5, 'r' || g % 2, repeat('p', 1000) FROM generate_series(5000, 5999) g; }
step s2p	{ SELECT sift.promote('iso_fac_cat') >= 0 AS promoted_cat, sift.promote('iso_fac_body') >= 0 AS promoted_body; }
step s2m	{ SELECT sift.merge('iso_fac_cat', true) >= 0 AS merged_cat, sift.merge('iso_fac_body', true) >= 0 AS merged_body; }

session s3
step s3check	{ SELECT iso_facets_ok('common') AS ok_common, iso_facets_ok('w1 OR w2') AS ok_or, iso_facets_ok('w3 AND NOT w4') AS ok_not; }
step s3verify	{ SELECT iso_verify('iso_fac_cat') AS verified_cat, iso_verify('iso_fac_body') AS verified_body; }

# uncommitted and rolled back inserts are not counted, committed ones are
permutation s2b s2i s3check s2a s3check s2b s2i s2c s3check
# a snapshot keeps the rows deleted and updated after it, through VACUUM, until it ends
permutation s1b s2d s2u s1check s2v s1check s1c s2v s3check s3verify
# freed line pointers get new rows with other values while old snapshots and entries are around
permutation s2d s2v s1b s2r s1check s2p s1check s2m s1check s1c s3check s3verify
