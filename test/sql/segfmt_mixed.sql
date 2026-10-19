-- Segment formats 2 and 3 side by side (docs/perf/BM25-SIZE.md 5.0): the same corpus indexed in each format holds the same postings and answers
-- every query alike, an index of format 2 only keeps the metapage at 1.2 (the 3.0 library reads it), a format 3 segment raises it to 1.3, and an index
-- of both is verified, queried, vacuumed and merged into one segment of format 3.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- 30000 rows on about 300 heap pages: a term in every row (GROUPED), mid-sized lists, a term twice in some rows, terms in one row (inline), in two rows (inline),
-- rows of 5 to 45 tokens
CREATE TABLE src AS
SELECT g AS id, concat_ws(' ', 'common', 'w' || g % 97, 'mid' || g % 11, CASE WHEN g % 5 = 0 THEN 'five' END,
       CASE WHEN g % 7 = 0 THEN 'seven seven seven' END, 'u' || g, 'p' || g / 2, repeat('filler ', g % 40)) AS body
FROM generate_series(1, 30000) g;
-- terms of 200 postings and more are GROUPED: the rare ones as containers of 150 postings on 256 pages, the mid-sized ones with 7 on a page, 'common' dense
SET sift.grouped_min_df = 200;
CREATE TABLE t2 AS SELECT * FROM src ORDER BY id;
CREATE TABLE t3 AS SELECT * FROM src ORDER BY id;
SET sift.test_segfmt = 2;
CREATE INDEX t2_idx ON t2 USING sift (body);
CREATE INDEX t2_off ON t2 USING sift (body) WITH (positions = off);
SET sift.test_segfmt = 3;
CREATE INDEX t3_idx ON t3 USING sift (body);
CREATE INDEX t3_off ON t3 USING sift (body) WITH (positions = off);

SELECT 't2_idx' AS idx, gen > 0 AS built, format FROM sift._test_segment_formats('t2_idx') WHERE kind = 'immutable'
UNION ALL SELECT 't3_idx', gen > 0, format FROM sift._test_segment_formats('t3_idx') WHERE kind = 'immutable'
UNION ALL SELECT 't2_idx metapage minor', true, format FROM sift._test_segment_formats('t2_idx') WHERE kind = 'metapage minor'
UNION ALL SELECT 't3_idx metapage minor', true, format FROM sift._test_segment_formats('t3_idx') WHERE kind = 'metapage minor';

SELECT sift.verify('t2_idx', true), sift.verify('t3_idx', true), sift.verify('t2_off', true), sift.verify('t3_off', true);
SELECT (SELECT bytes FROM sift._test_segment_parts('t3_idx') WHERE part = 'postings: GROUPED containers')
       < (SELECT bytes FROM sift._test_segment_parts('t2_idx') WHERE part = 'postings: GROUPED containers') AS packed_containers_smaller;

-- the postings of an index by document id (the tids of two tables differ when their rows were not placed alike), the rows that are gone left out
CREATE FUNCTION posts(idx regclass, tbl regclass) RETURNS TABLE (term text, id int, tf int, dl int) LANGUAGE plpgsql AS $f$
BEGIN
	RETURN QUERY EXECUTE format('SELECT p.term, t.id, p.tf, p.dl FROM sift._test_postings(%L) p JOIN %s t ON t.ctid = p.tid', idx, tbl);
END $f$;

-- the same postings, positions on and off, term by term and document by document
SELECT (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('t2_idx', 't2') EXCEPT SELECT term, id, tf, dl FROM posts('t3_idx', 't3')) x) AS only_2,
       (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('t3_idx', 't3') EXCEPT SELECT term, id, tf, dl FROM posts('t2_idx', 't2')) x) AS only_3,
       (SELECT count(*) FROM posts('t3_idx', 't3')) AS postings,
       (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('t2_off', 't2') EXCEPT SELECT term, id, tf, dl FROM posts('t3_off', 't3')) x) AS off_only_2,
       (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('t3_off', 't3') EXCEPT SELECT term, id, tf, dl FROM posts('t2_off', 't2')) x) AS off_only_3;

-- the same answers: counts, a phrase (positions or heap recheck), prefix and fuzzy leaves, and ranked scores to the bit
CREATE FUNCTION answers(tbl regclass, heap bool DEFAULT false) RETURNS TABLE (q text, n bigint, h text) LANGUAGE plpgsql AS $f$
DECLARE
	qs text[] := ARRAY['common', 'common AND w7', 'mid3 OR mid5', 'five AND NOT seven', '"common w7"', '"seven seven"', 'u1234', 'p77 OR p78', 'p77', 'w1*', 'filler~1'];
	i int;
BEGIN
	PERFORM set_config('enable_indexscan', CASE WHEN heap THEN 'off' ELSE 'on' END, true), set_config('enable_bitmapscan', CASE WHEN heap THEN 'off' ELSE 'on' END, true),
		set_config('sift.enable_scan', CASE WHEN heap THEN 'off' ELSE 'on' END, true), set_config('sift.enable_count', CASE WHEN heap THEN 'off' ELSE 'on' END, true);
	FOR i IN 1..array_length(qs, 1) LOOP
		q := qs[i];
		EXECUTE format('SELECT count(*), coalesce(md5(string_agg(id::text, '','' ORDER BY id)), '''') FROM %s WHERE body @@> %L', tbl, qs[i]) INTO n, h;
		RETURN NEXT;
	END LOOP;
	FOR i IN 1..array_length(qs, 1) LOOP
		EXIT WHEN heap;
		q := 'ranked ' || qs[i];
		EXECUTE format('SELECT count(*), md5(string_agg(id || '':'' || round(s::numeric, 6), '','' ORDER BY s DESC, id)) FROM
			(SELECT id, sift.score(ctid) AS s FROM %s WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT 25) r', tbl, qs[i]) INTO n, h;
		RETURN NEXT;
	END LOOP;
END $f$;

SELECT count(*) AS queries, count(*) FILTER (WHERE a.n = b.n AND a.h = b.h) AS same_t2_t3 FROM answers('t2') a JOIN answers('t3') b USING (q);
SELECT count(*) AS queries, count(*) FILTER (WHERE a.n = b.n AND a.h = b.h) AS same_t3_heap FROM answers('t3') a JOIN answers('t3', true) b USING (q);

-- the dl rows of format 3 are 4, 8 or 16 bits by superblock: five rows per heap page (a pad of a fixed size) in three runs of 1300 rows, whose
-- documents hold 5, 62 and 302 tokens (page 256 of the table starts the second superblock)
CREATE TABLE wd AS
SELECT g AS id, b AS body, repeat('x', 1500 - length(b)) AS pad
FROM (SELECT g, concat_ws(' ', 'common', 'w' || g % 97, repeat('t ', CASE WHEN g <= 1300 THEN 3 WHEN g <= 2600 THEN 60 ELSE 300 END)) AS b
      FROM generate_series(1, 3900) g) x;
CREATE TABLE wd2 AS SELECT * FROM wd ORDER BY id;
SET sift.test_segfmt = 2;
CREATE INDEX wd2_idx ON wd2 USING sift (body);
SET sift.test_segfmt = 3;
CREATE INDEX wd3_idx ON wd USING sift (body);
SELECT sift.verify('wd2_idx', true), sift.verify('wd3_idx', true);
SELECT (SELECT bytes FROM sift._test_segment_parts('wd3_idx') WHERE part = 'docmap: dl rows')
       < (SELECT bytes FROM sift._test_segment_parts('wd2_idx') WHERE part = 'docmap: dl rows') AS narrower_rows,
       (SELECT bytes FROM sift._test_segment_parts('wd3_idx') WHERE part = 'docmap: dl rows') > 0 AS has_rows;
SELECT (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('wd2_idx', 'wd2') EXCEPT SELECT term, id, tf, dl FROM posts('wd3_idx', 'wd')) x) AS only_2,
       (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('wd3_idx', 'wd') EXCEPT SELECT term, id, tf, dl FROM posts('wd2_idx', 'wd2')) x) AS only_3,
       (SELECT count(DISTINCT dl) FROM posts('wd3_idx', 'wd')) AS dls;
SELECT count(*) AS queries, count(*) FILTER (WHERE a.n = b.n AND a.h IS NOT DISTINCT FROM b.h) AS same_wd FROM answers('wd2') a JOIN answers('wd') b USING (q);
DROP TABLE wd, wd2;

-- mixed: format 2 for the first half, format 3 for the second (a promoted write buffer), then deletes, VACUUM and a merge
CREATE TABLE tm AS SELECT * FROM src WHERE id <= 15000 ORDER BY id;
SET sift.test_segfmt = 2;
CREATE INDEX tm_idx ON tm USING sift (body) WITH (vacuum_segments = 8, dead_rewrite_pct = 100);
SET sift.test_segfmt = 3;
SET sift.async_maintenance = off;
INSERT INTO tm SELECT * FROM src WHERE id > 15000 ORDER BY id;
SELECT sift.promote('tm_idx') > 0 AS promoted;
SELECT format, count(*) AS segments FROM sift._test_segment_formats('tm_idx') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT format FROM sift._test_segment_formats('tm_idx') WHERE kind = 'metapage minor';
SELECT sift.verify('tm_idx', true);
SELECT (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('tm_idx', 'tm') EXCEPT SELECT term, id, tf, dl FROM posts('t3_idx', 't3')) x) AS only_mixed,
       (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('t3_idx', 't3') EXCEPT SELECT term, id, tf, dl FROM posts('tm_idx', 'tm')) x) AS only_t3;
SELECT count(*) AS queries, count(*) FILTER (WHERE a.n = b.n AND a.h = b.h) AS same_mixed_t3 FROM answers('tm') a JOIN answers('t3') b USING (q);

-- VACUUM skips a page whose cleanup lock it cannot take at once (the checkpointer or the background writer holds a pin) unless it has to freeze
-- the page: FREEZE makes it wait, so the dead rows of both tables go and their statistics are the same (3.7% of runs differed under load)
DELETE FROM tm WHERE id % 4 = 1;
DELETE FROM t3 WHERE id % 4 = 1;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) tm;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) t3;
SELECT sift.verify('tm_idx', true);
SELECT count(*) AS queries, count(*) FILTER (WHERE a.n = b.n AND a.h = b.h) AS same_after_vacuum FROM answers('tm') a JOIN answers('t3') b USING (q);

SELECT sift.merge('tm_idx', true) > 0 AS merged, sift.merge('t3_idx', true) > 0 AS merged_t3;
SELECT format, count(*) AS segments FROM sift._test_segment_formats('tm_idx') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT sift.verify('tm_idx', true);
SELECT count(*) AS queries, count(*) FILTER (WHERE a.n = b.n AND a.h = b.h) AS same_after_merge FROM answers('tm') a JOIN answers('t3') b USING (q);
SELECT (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('tm_idx', 'tm') EXCEPT SELECT term, id, tf, dl FROM posts('t3_idx', 't3')) x) AS only_merged,
       (SELECT count(*) FROM (SELECT term, id, tf, dl FROM posts('t3_idx', 't3') EXCEPT SELECT term, id, tf, dl FROM posts('tm_idx', 'tm')) x) AS only_t3;

-- VACUUM rewrites a mostly dead segment of format 2 in format 3
CREATE TABLE vr AS SELECT * FROM src WHERE id <= 6000 ORDER BY id;
SET sift.test_segfmt = 2;
CREATE INDEX vr_idx ON vr USING sift (body) WITH (build_segments = 1, dead_rewrite_pct = 50);
SET sift.test_segfmt = 3;
SELECT format, count(*) AS segments FROM sift._test_segment_formats('vr_idx') WHERE kind = 'immutable' GROUP BY 1;
DELETE FROM vr WHERE id % 10 < 7;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) vr;
SELECT format, count(*) AS segments, sum(ndocs) AS docs
FROM sift._test_segment_formats('vr_idx') f JOIN sift.segments('vr_idx') s USING (gen) WHERE f.kind = 'immutable' GROUP BY 1;
SELECT sift.verify('vr_idx', true);
SELECT count(*) AS hits FROM vr WHERE body @@> 'common';
DROP TABLE vr;

DROP FUNCTION answers(regclass, bool);
DROP FUNCTION posts(regclass, regclass);
DROP TABLE src, t2, t3, tm;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
