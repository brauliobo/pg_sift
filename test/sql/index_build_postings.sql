-- Postings round trip: every (term, tid, tf, dl) decoded from the index
-- equals an independent computation from the heap through sift.tokenize,
-- for one or several segments, in memory or through sorted runs.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- docs: a query returning (tid, body) of the indexed rows. The comparison runs in 8 batches of heap blocks, summed: the whole
-- corpus is compared, but the joins and sorts of a batch fit a small temp file instead of half a gigabyte.
CREATE FUNCTION roundtrip(idx regclass, docs text)
RETURNS TABLE (postings bigint, missing bigint, extra bigint, differ bigint)
LANGUAGE plpgsql AS $f$
DECLARE
	b int;
	r record;
BEGIN
	postings := 0; missing := 0; extra := 0; differ := 0;
	FOR b IN 0..7 LOOP
		EXECUTE format($q$
			WITH d AS (SELECT * FROM (%s) q WHERE (q.tid::text::point)[0]::int %% 8 = %s),
			tok AS (SELECT d.tid, t.token FROM d, sift.tokenize(d.body, %L) t),
			len AS (SELECT tid, least(count(*), 65535)::int AS dl FROM tok GROUP BY tid),
			want AS (SELECT tok.token AS term, tok.tid, least(count(*), 255)::int AS tf, len.dl
					 FROM tok JOIN len USING (tid) GROUP BY 1, 2, 4),
			got AS (SELECT term, tid, tf, dl FROM sift._test_postings(%L) WHERE (tid::text::point)[0]::int %% 8 = %s)
			SELECT count(*) AS n, count(*) FILTER (WHERE g.term IS NULL) AS m,
				   count(*) FILTER (WHERE w.term IS NULL) AS x,
				   count(*) FILTER (WHERE g.tf <> w.tf OR g.dl <> w.dl) AS dd
			FROM want w FULL JOIN got g ON g.term = w.term AND g.tid = w.tid$q$, docs, b, idx, idx, b) INTO r;
		postings := postings + r.n; missing := missing + r.m; extra := extra + r.x; differ := differ + r.dd;
	END LOOP;
	RETURN NEXT;
END
$f$;

-- 100k documents, Zipf-like vocabulary of 20k words, a term in every
-- document (GROUPED), repeated words (tf escapes, tf and dl saturation)
SELECT setseed(0.42);
CREATE TABLE z (id int, body text);
INSERT INTO z
SELECT i, 'common ' || (SELECT string_agg('w' || floor(exp(random() * ln(20000)))::int, ' ')
						FROM generate_series(1, 1 + (random() * 60)::int + i % 2))
FROM generate_series(1, 100000) i;
UPDATE z SET body = body || repeat(' rep', id % 40) WHERE id % 97 = 0;
UPDATE z SET body = body || repeat(' many', 300) || repeat(' x', 70000) WHERE id = 5000;
VACUUM FULL z;

CREATE INDEX z_idx ON z USING sift (body);
SELECT gen, ndocs, sum_dl, nterms, sb_lo, sb_hi FROM sift.segments('z_idx');
SELECT * FROM sift.segment_postings('z_idx');
SELECT sift.verify('z_idx');
SELECT * FROM roundtrip('z_idx', 'SELECT ctid AS tid, body FROM z');

-- Bounded memory: 1 MB spills sorted runs that merge into one segment
SET maintenance_work_mem = '1MB';
REINDEX INDEX z_idx;
SELECT gen, ndocs, sum_dl, nterms FROM sift.segments('z_idx');
SELECT sift.verify('z_idx');
SELECT * FROM roundtrip('z_idx', 'SELECT ctid AS tid, body FROM z');

-- Runs and forced heap ranges: several segments
CREATE INDEX z_seg ON z USING sift (body) WITH (build_segments = 3);
SELECT gen, ndocs, sum_dl, nterms, sb_lo, sb_hi FROM sift.segments('z_seg');
SELECT sift.verify('z_seg');
SELECT * FROM roundtrip('z_seg', 'SELECT ctid AS tid, body FROM z');
RESET maintenance_work_mem;

-- Partial and expression indexes, other analyzer options
CREATE INDEX z_part ON z USING sift (body) WHERE id % 10 = 3;
SELECT sift.verify('z_part');
SELECT * FROM roundtrip('z_part', 'SELECT ctid AS tid, body FROM z WHERE id % 10 = 3');
CREATE INDEX z_expr ON z USING sift ((upper(substr(body, 1, 40)) || ' Ünïcödé ' || id))
	WITH (stemmer = 'en', accent_folding = false);
SELECT sift.verify('z_expr');
SELECT * FROM roundtrip('z_expr', $$SELECT ctid AS tid, upper(substr(body, 1, 40)) || ' Ünïcödé ' || id AS body FROM z$$);

-- UTF-8, emoji, duplicates
CREATE TABLE u (body text);
INSERT INTO u SELECT CASE i % 4 WHEN 0 THEN 'Crème brûlée ' || i WHEN 1 THEN '東京 🗼🇯🇵 👍🏽 ' || (i % 7)
	WHEN 2 THEN 'same same same' ELSE 'ǅemal Straße ﬁ ' || (i % 3) END FROM generate_series(1, 2000) i;
CREATE INDEX u_idx ON u USING sift (body);
SELECT sift.verify('u_idx');
SELECT * FROM roundtrip('u_idx', 'SELECT ctid AS tid, body FROM u');

DROP TABLE z, u;
DROP FUNCTION roundtrip;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
