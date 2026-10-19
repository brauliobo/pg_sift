# sift.facet_counts while rows change: the counts of a snapshot are the GROUP BY of the same snapshot (terms with a top N and its other
# row, range buckets and the hits row, over the column scan with the search as its row selection), whether the writes are uncommitted,
# deleted and vacuumed, updated, merged away or reuse the freed line pointers.
# requires: build insert vacuum scan promote merge verify facets

setup
{
	CREATE TABLE iso_fcw (id int PRIMARY KEY, body text, cat text, price int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_fcw SELECT g, 'common w' || g % 5, 'c' || g % 7, g % 400, repeat('p', 1000) FROM generate_series(1, 3000) g;
	CREATE INDEX iso_fcw_body ON iso_fcw USING sift (body) WITH (write_buffer_limit = 64);
	CREATE INDEX iso_fcw_col ON iso_fcw USING sift_col (cat, price) WITH (write_buffer_limit = 64);
	CREATE FUNCTION iso_fcw_ok(q text) RETURNS bool LANGUAGE plpgsql
		SET sift.enable_scan = off SET sift.enable_count = off SET enable_bitmapscan = off SET enable_indexscan = off SET enable_seqscan = off AS $$
	DECLARE
		got text;
		want text;
	BEGIN
		SELECT string_agg(coalesce(facet, '') || ':' || kind || ':' || coalesce(value, '-') || ':' || coalesce(lo, '-') || ':' || count, ' ' ORDER BY ord) INTO got
			FROM sift.facet_counts('iso_fcw', 'body', q,
				jsonb_build_array(jsonb_build_object('name', 'cat', 'expr', 'cat', 'size', 3),
					jsonb_build_object('name', 'p', 'expr', 'price', 'edges', jsonb_build_array(100, 300))), NULL, true) WITH ORDINALITY f(facet, kind, value, lo, hi, count, ord);
		WITH g AS (SELECT cat, count(*) n FROM iso_fcw WHERE body @@> q GROUP BY cat),
			 r AS (SELECT cat, n, row_number() OVER (ORDER BY n DESC, cat COLLATE "C") rn FROM g)
		SELECT string_agg(s, ' ' ORDER BY o) INTO want FROM (
			SELECT 'cat:value:' || cat || ':-:' || n s, rn o FROM r WHERE rn <= 3
			UNION ALL SELECT 'cat:other:-:' || count(*) || ':' || sum(n), 100 FROM r WHERE rn > 3 HAVING count(*) > 0
			UNION ALL SELECT 'p:bucket:[,100):-:' || count(*), 200 FROM iso_fcw WHERE body @@> q AND price < 100
			UNION ALL SELECT 'p:bucket:[100,300):100:' || count(*), 201 FROM iso_fcw WHERE body @@> q AND price >= 100 AND price < 300
			UNION ALL SELECT 'p:bucket:[300,):300:' || count(*), 202 FROM iso_fcw WHERE body @@> q AND price >= 300
			UNION ALL SELECT ':hits:-:-:' || count(*), 0 FROM iso_fcw WHERE body @@> q) x;
		RETURN got IS NOT DISTINCT FROM want;
	END $$;
	CREATE FUNCTION iso_fcw_colverify() RETURNS bool LANGUAGE plpgsql AS $$ BEGIN PERFORM sift.col_verify('iso_fcw_col', true); RETURN true; END $$;
}

teardown
{
	DROP TABLE iso_fcw;
	DROP FUNCTION iso_fcw_ok(text);
	DROP FUNCTION iso_fcw_colverify();
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS rows FROM iso_fcw; }
step s1check	{ SELECT iso_fcw_ok('common') AS ok_common, iso_fcw_ok('w1 OR w2') AS ok_or, iso_fcw_ok('w3 AND NOT w4') AS ok_not; }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2c	{ COMMIT; }
step s2a	{ ROLLBACK; }
step s2i	{ INSERT INTO iso_fcw SELECT g, 'common w' || g % 5, 'n' || g % 3, g % 500, repeat('p', 1000) FROM generate_series(4000, 4999) g; }
step s2d	{ DELETE FROM iso_fcw WHERE id % 3 = 0; }
step s2u	{ UPDATE iso_fcw SET cat = 'moved', price = price + 250 WHERE id % 4 = 0; }
step s2v	{ VACUUM iso_fcw; }
step s2r	{ INSERT INTO iso_fcw SELECT g, 'common w' || g % 5, 'r' || g % 2, g % 700, repeat('p', 1000) FROM generate_series(5000, 5999) g; }
step s2p	{ SELECT sift.promote('iso_fcw_body') >= 0 AS promoted_body, sift.col_promote('iso_fcw_col') >= 0 AS promoted_col; }
step s2m	{ SELECT sift.merge('iso_fcw_body', true) >= 0 AS merged_body, sift.col_merge('iso_fcw_col', true) >= 0 AS merged_col; }

session s3
step s3check	{ SELECT iso_fcw_ok('common') AS ok_common, iso_fcw_ok('w1 OR w2') AS ok_or, iso_fcw_ok('w3 AND NOT w4') AS ok_not; }
step s3verify	{ SELECT iso_verify('iso_fcw_body') AS verified_body, iso_fcw_colverify() AS verified_col; }

# uncommitted and rolled back inserts are not counted, committed ones are
permutation s2b s2i s3check s2a s3check s2b s2i s2c s3check
# a snapshot keeps the rows deleted and updated after it, through VACUUM, until it ends
permutation s1b s2d s2u s1check s2v s1check s1c s2v s3check s3verify
# freed line pointers get new rows with other values while old snapshots and entries are around
permutation s2d s2v s1b s2r s1check s2p s1check s2m s1check s1c s3check s3verify
