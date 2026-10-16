-- Representation thresholds: inline (df <= 2), LIST, GROUPED from
-- sift.grouped_min_df (65536 in production), across superblocks, with
-- block bounds and tf escapes; round trip against sift.tokenize.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE FUNCTION roundtrip(idx regclass, docs text)
RETURNS TABLE (postings bigint, missing bigint, extra bigint, differ bigint)
LANGUAGE plpgsql AS $f$
BEGIN
	RETURN QUERY EXECUTE format($q$
		WITH d AS (%s),
		tok AS (SELECT d.tid, t.token FROM d, sift.tokenize(d.body, %L) t),
		len AS (SELECT tid, least(count(*), 65535)::int AS dl FROM tok GROUP BY tid),
		want AS (SELECT tok.token AS term, tok.tid, least(count(*), 255)::int AS tf, len.dl
				 FROM tok JOIN len USING (tid) GROUP BY 1, 2, 4),
		got AS (SELECT term, tid, tf, dl FROM sift._test_postings(%L))
		SELECT count(*), count(*) FILTER (WHERE g.term IS NULL),
			   count(*) FILTER (WHERE w.term IS NULL),
			   count(*) FILTER (WHERE g.tf <> w.tf OR g.dl <> w.dl)
		FROM want w FULL JOIN got g ON g.term = w.term AND g.tid = w.tid$q$, docs, idx, idx);
END
$f$;

SHOW sift.grouped_min_df;

-- Fillfactor 10: a few rows per page over more than two superblocks
SELECT setseed(0.7);
CREATE TABLE g (id int, body text) WITH (fillfactor = 10);
INSERT INTO g
SELECT i, 'all ' || CASE WHEN i <= 2 THEN 'two ' ELSE '' END || CASE WHEN i <= 3 THEN 'three ' ELSE '' END
	|| CASE WHEN i % 2 = 0 THEN 'even ' ELSE '' END || CASE WHEN i % 500 = 0 THEN 'rare ' ELSE '' END
	|| repeat('tf ', (i % 300) + 1)
	|| (SELECT string_agg('v' || floor(exp(random() * ln(3000)))::int, ' ') FROM generate_series(1, 5 + i % 9))
FROM generate_series(1, 6000) i;
SELECT pg_relation_size('g') / 8192 > 512 AS three_superblocks;

-- Production thresholds: everything below 65536 is inline or LIST
CREATE INDEX g_list ON g USING sift (body) WITH (positions = off);
SELECT df_class, terms, inline_terms, list_terms, grouped_terms FROM sift.segment_postings('g_list');
SELECT sift.verify('g_list');
SELECT * FROM roundtrip('g_list', 'SELECT ctid AS tid, body FROM g');

-- GROUPED from df 3: inline stays at df <= 2
SET sift.grouped_min_df = 3;
CREATE INDEX g_grouped ON g USING sift (body);
SELECT df_class, terms, inline_terms, list_terms, grouped_terms FROM sift.segment_postings('g_grouped');
SELECT sift.verify('g_grouped');
SELECT * FROM roundtrip('g_grouped', 'SELECT ctid AS tid, body FROM g');

-- The boundary: df 3 LIST, df 4 GROUPED
SET sift.grouped_min_df = 4;
CREATE INDEX g_edge ON g USING sift (body);
SELECT df_class, terms, inline_terms, list_terms, grouped_terms FROM sift.segment_postings('g_edge');
SELECT sift.verify('g_edge');
SELECT * FROM roundtrip('g_edge', 'SELECT ctid AS tid, body FROM g');

-- Several segments sharing superblocks, GROUPED
SET sift.grouped_min_df = 100;
CREATE INDEX g_segs ON g USING sift (body) WITH (build_segments = 5);
SELECT gen, ndocs, nterms, sb_lo, sb_hi FROM sift.segments('g_segs');
SELECT sift.verify('g_segs');
SELECT * FROM roundtrip('g_segs', 'SELECT ctid AS tid, body FROM g');
RESET sift.grouped_min_df;

-- sift.verify catches damage: header totals, a docmap record, postings bytes
SELECT header AS hdr FROM sift._test_segments('g_list') \gset
SELECT sift._test_poke('g_list', :hdr, 24 + 8, 5999, 8);
SELECT sift.verify('g_list');
REINDEX INDEX g_list;
SELECT sift._test_poke('g_list', 2, 24, 12345, 4);
SELECT sift.verify('g_list');
REINDEX INDEX g_list;
SELECT sift._test_poke('g_list', 6, 4000, -1, 8);
SELECT sift.verify('g_list');
REINDEX INDEX g_list;
SELECT sift.verify('g_list');

DROP TABLE g;
DROP FUNCTION roundtrip;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
