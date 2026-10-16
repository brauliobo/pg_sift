-- Filter masks (src/sift_filter.c): a TIDBitmap with exact and lossy pages as page and offset masks per
-- superblock, under a byte budget. The mask keeps every tuple of the bitmap; a page that does not fit the budget,
-- like a lossy one, keeps all its offsets, so the mask is never smaller than the bitmap and never reaches a page
-- the bitmap does not. (A lossy chunk makes the first page of its range lossy too: a superset, so the exact
-- answers below leave the pages 0, 256 and 512 out.)
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- three superblocks (blocks 0..767): tuples on some pages, some pages lossy, probes everywhere
CREATE TEMP TABLE ft AS SELECT DISTINCT ((g * 37) % 384) * 2 AS b, 1 + (g * 101) % 291 AS o FROM generate_series(1, 3000) g;
CREATE TEMP TABLE fl AS SELECT DISTINCT (g * 61) % 768 AS b FROM generate_series(1, 40) g;
CREATE TEMP TABLE fp AS SELECT g AS n, (g * 13) % 768 AS b, 1 + (g * 7) % 291 AS o FROM generate_series(1, 20000) g;
SELECT (SELECT count(*) FROM ft) AS tuples, (SELECT count(DISTINCT b) FROM ft) AS tuple_pages,
	(SELECT count(*) FROM fl) AS lossy_pages, (SELECT count(*) FROM ft WHERE b IN (SELECT b FROM fl)) AS tuples_on_lossy;

CREATE FUNCTION filter_check(maxbytes int8) RETURNS TABLE (sbs int8, exact_pages int8, lossy_pages int8, tuples int8,
	block_mismatches int8, missed int8, beyond_pages int8, exact_answers bool, page_answers bool) LANGUAGE sql AS $$
	WITH r AS (
		SELECT * FROM sift._test_filter((SELECT array_agg(format('(%s,%s)', b, o)::tid) FROM ft),
			(SELECT array_agg(b::int8) FROM fl), maxbytes, (SELECT array_agg(format('(%s,%s)', b, o)::tid ORDER BY n) FROM fp))
	), p AS (
		SELECT fp.n, fp.b, h AS has,
			EXISTS (SELECT 1 FROM ft WHERE ft.b = fp.b AND ft.o = fp.o) OR fp.b IN (SELECT b FROM fl) AS in_bitmap,
			fp.b IN (SELECT b FROM ft UNION SELECT b FROM fl) AS on_page
		FROM r, unnest(r.has) WITH ORDINALITY u(h, n) JOIN fp USING (n)
	)
	SELECT r.superblocks, r.exact_pages, r.lossy_pages, r.tuples, r.block_mismatches,
		(SELECT count(*) FROM p WHERE in_bitmap AND NOT has), (SELECT count(*) FROM p WHERE has AND NOT on_page),
		(SELECT bool_and(has = in_bitmap) FILTER (WHERE b % 256 <> 0) FROM p), (SELECT bool_and(has = on_page) FROM p)
	FROM r
$$;

-- room for every page: the mask is the bitmap
SELECT * FROM filter_check(1 << 30);
-- no room for offsets: every page is whole
SELECT * FROM filter_check(0);
-- room for 100 exact pages: the first 100 in block order stay exact
SELECT * FROM filter_check(64 * 100);
SELECT * FROM filter_check(64 * 100 - 1);

-- an empty bitmap keeps nothing
SELECT superblocks, exact_pages, lossy_pages, tuples, has, block_mismatches
FROM sift._test_filter('{}', '{}', 1 << 20, ARRAY['(5,5)']::tid[]);
-- tuples at the last offset and at both ends of a superblock, and a lossy page that makes page 256, the first of its
-- chunk, lossy too: (256,1) is kept
SELECT superblocks, exact_pages, lossy_pages, tuples, has, block_mismatches
FROM sift._test_filter(ARRAY['(0,291)', '(255,1)', '(256,291)']::tid[], ARRAY[300]::int8[], 1 << 20,
	ARRAY['(0,291)', '(0,290)', '(255,1)', '(255,2)', '(256,291)', '(256,1)', '(300,1)', '(300,291)', '(301,1)', '(1000,1)']::tid[]);

DROP FUNCTION filter_check(int8);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
