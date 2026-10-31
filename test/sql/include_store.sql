-- The INCLUDE columns of a sift index are stored per doc in the segments (format 5) and the reader returns what the heap tuple held when the
-- segment was written: after the build, a promotion, a merge, a VACUUM that rewrites a segment and a REINDEX, and for every type that is kept.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE docs (id int, body text, category_id int, created_at date, active bool, label text, counter_a int8, seen_at timestamptz);
INSERT INTO docs
SELECT g, 'dim_a' || g % 7 || ' w' || g % 30, CASE WHEN g % 11 = 0 THEN NULL ELSE g % 97 END, DATE '2026-01-01' + g % 300, g % 3 = 0,
	'label_a' || g % 5, g::int8 * 1000000000, TIMESTAMPTZ '2026-01-01 00:00:00+00' + g * interval '1 minute'
FROM generate_series(1, 30000) g;
CREATE INDEX docs_idx ON docs USING sift (body) INCLUDE (category_id, created_at, active, label, counter_a, seen_at);

-- what the index holds of every doc against the heap: a column of a kept type reads back its value or NULL, the text column has no rows
CREATE FUNCTION mismatches() RETURNS TABLE (col int, state text, n bigint) LANGUAGE sql AS $$
	WITH h AS (
		SELECT (ctid::text::point)[0]::int8 AS blk, (ctid::text::point)[1]::int AS off, category_id, created_at - DATE '2026-01-01' AS created_at,
			active::int AS active, counter_a FROM docs),
	d AS (SELECT * FROM sift._test_attr_dump('docs_idx'))
	SELECT d.col, d.state, count(*) FROM d JOIN h USING (blk, off)
	WHERE d.col <> 3
	  AND d.state IS DISTINCT FROM (CASE WHEN CASE d.col WHEN 0 THEN h.category_id::int8 WHEN 1 THEN h.created_at WHEN 2 THEN h.active WHEN 4 THEN h.counter_a WHEN 5 THEN 1 END IS NULL THEN 'null' ELSE 'value' END)
	GROUP BY 1, 2 ORDER BY 1, 2
$$;
CREATE FUNCTION value_mismatches() RETURNS bigint LANGUAGE sql AS $$
	WITH h AS (
		SELECT (ctid::text::point)[0]::int8 AS blk, (ctid::text::point)[1]::int AS off, category_id, (created_at - DATE '2000-01-01') AS created_at,
			active::int AS active, counter_a, ((extract(epoch FROM seen_at) - 946684800) * 1000000)::int8 AS seen_at FROM docs),
	d AS (SELECT * FROM sift._test_attr_dump('docs_idx') WHERE state = 'value')
	SELECT count(*) FROM d JOIN h USING (blk, off)
	WHERE d.val IS DISTINCT FROM (CASE d.col WHEN 0 THEN h.category_id::int8 WHEN 1 THEN h.created_at WHEN 2 THEN h.active WHEN 4 THEN h.counter_a WHEN 5 THEN h.seen_at END)
$$;
CREATE FUNCTION docs_held() RETURNS TABLE (cols int, docs bigint, formats text) LANGUAGE sql AS $$
	SELECT (SELECT count(DISTINCT col)::int FROM sift._test_attr_dump('docs_idx')), (SELECT count(DISTINCT (blk, off)) FROM sift._test_attr_dump('docs_idx')),
		(SELECT string_agg(DISTINCT format::text, ',') FROM sift._test_segment_formats('docs_idx') WHERE kind = 'immutable')
$$;

SELECT * FROM docs_held();
SELECT * FROM mismatches();
SELECT value_mismatches();
-- the text column has no rows (it passes every test) and the others have
SELECT col, state, count(*) FROM sift._test_attr_dump('docs_idx') WHERE col = 3 GROUP BY 1, 2;

-- a promotion of rows inserted after the build, a merge of the two, a VACUUM that drops dead docs, and a REINDEX
INSERT INTO docs SELECT 30000 + g, 'dim_a' || g % 7 || ' w' || g % 30, g % 53, DATE '2026-02-01' + g % 100, g % 2 = 0, 'label_a' || g % 5, g, TIMESTAMPTZ '2026-03-01+00' + g * interval '1 second'
FROM generate_series(1, 20000) g;
SELECT sift.promote('docs_idx') >= 0 AS promoted;
SELECT * FROM docs_held();
SELECT * FROM mismatches();
SELECT value_mismatches();
SELECT sift.merge('docs_idx') >= 0 AS merged;
SELECT * FROM docs_held();
SELECT value_mismatches();
DELETE FROM docs WHERE id % 4 = 0;
VACUUM docs;
SELECT sift.merge('docs_idx') >= 0 AS merged_again;
SELECT * FROM mismatches();
SELECT value_mismatches();
REINDEX INDEX docs_idx;
SELECT * FROM docs_held();
SELECT value_mismatches();

DROP FUNCTION mismatches(), value_mismatches(), docs_held();
DROP TABLE docs;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
