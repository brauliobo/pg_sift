-- A superblock whose INCLUDE column holds no value that passes a test is not read at all (the ranker takes its range from the header of its record), so
-- on a table whose columns follow the heap order a search with a test scans the superblocks of the range only. The answers must stay those of the same
-- search on a copy of the table whose index has no INCLUDE columns, with NULL stretches, rows of the write buffer, updated and deleted rows, a promotion,
-- a merge, a VACUUM and a REINDEX; a superblock with a doc whose heap tuple was gone when its segment was written is read whatever its range.
CREATE EXTENSION pg_sift;

-- 40,000 rows of about 13 to a page: 12 superblocks of 256 pages, each column ascending with id
CREATE TABLE docs (id int PRIMARY KEY, body text, category_id int, created_at date, counter_a int8, tier smallint, pad text);
INSERT INTO docs
SELECT g, 'common dim_a' || g % 7 || ' w' || g % 30,
	CASE WHEN g BETWEEN 9000 AND 11999 OR g % 97 = 0 THEN NULL ELSE g / 400 END,
	CASE WHEN g BETWEEN 20000 AND 21999 THEN NULL ELSE DATE '2026-01-01' + g / 200 END,
	g::int8 * 10, CASE WHEN g % 13 = 0 THEN NULL ELSE (g / 10000)::smallint END, repeat('x', 600)
FROM generate_series(1, 40000) g;
CREATE TABLE docs_plain AS SELECT * FROM docs ORDER BY ctid;
CREATE INDEX docs_idx ON docs USING sift (body) INCLUDE (category_id, created_at, counter_a, tier);
CREATE INDEX docs_plain_idx ON docs_plain USING sift (body);
CREATE INDEX docs_cols ON docs (category_id, created_at, counter_a, tier);
CREATE INDEX docs_plain_cols ON docs_plain (category_id, created_at, counter_a, tier);
VACUUM ANALYZE docs, docs_plain;

CREATE TABLE preds (n serial, flt text);
INSERT INTO preds (flt) VALUES
	('created_at >= ''2026-06-01'''), ('created_at < ''2026-02-01'''), ('created_at = ''2026-03-03'''), ('created_at <> ''2026-03-03'''),
	('created_at > ''2026-07-10'''), ('created_at <= ''2026-01-20'''), ('created_at BETWEEN ''2026-04-01'' AND ''2026-04-30'''),
	('created_at IN (''2026-01-05'', ''2026-05-05'', ''2026-07-01'')'), ('created_at IS NULL'), ('created_at IS NOT NULL'),
	('created_at > ''2030-01-01'''), ('created_at < ''2020-01-01'''), ('created_at = ''2026-01-01'''), ('created_at >= ''2026-07-19'''),
	('counter_a > 300000'), ('counter_a <= 5000'), ('counter_a = 123450'), ('counter_a < 0'), ('counter_a IN (-1, -2)'), ('counter_a >= 399990'),
	('category_id = 7'), ('category_id IN (3, 50, 99)'), ('category_id >= 90'), ('category_id < 5'), ('category_id IS NULL'), ('category_id IS NOT NULL'),
	('tier = 2'), ('tier IS NOT NULL'), ('tier IN (0, 3)'), ('tier <> 1'), ('tier IS NULL'),
	('created_at >= ''2026-06-01'' AND category_id < 60'), ('created_at >= ''2026-06-01'' AND counter_a < 100000');

CREATE FUNCTION top_of(tbl regclass, q text, flt text, k int, OUT metrics_a real[], OUT ids int[]) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('WITH r AS (SELECT id, sift.score(ctid) AS s FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s)
		SELECT array_agg(s ORDER BY s DESC, id), array_agg(id ORDER BY id) FILTER (WHERE s > (SELECT min(s) FROM r)) FROM r', tbl, q, flt, k) INTO metrics_a, ids;
END $$;
CREATE FUNCTION answer_diffs(q text) RETURNS TABLE (flt text, k int) LANGUAGE sql AS $$
	SELECT p.flt, k.k FROM preds p, (VALUES (10), (1000)) k(k) WHERE top_of('docs', q, p.flt, k.k) IS DISTINCT FROM top_of('docs_plain', q, p.flt, k.k) ORDER BY 1, 2
$$;

-- the superblocks a search without a LIMIT reads, and the superblocks of the segments
CREATE FUNCTION scan_of(q text, OUT sbs bigint, OUT candidates bigint) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO j;
	SELECT (n ->> 'Superblocks')::bigint, (n ->> 'Candidates')::bigint INTO sbs, candidates
	FROM jsonb_path_query(j, 'strict $.**') n WHERE n ->> 'Custom Plan Provider' = 'SiftScan' LIMIT 1;
END $$;
CREATE FUNCTION scan_gaps(q text, tbl regclass, OUT flt text, OUT sbs bigint, OUT total bigint, OUT candidates bigint, OUT matching bigint) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	p record;
	s record;
BEGIN
	total := (scan_of(format('SELECT id FROM %s WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 1000000', tbl, q))).sbs;
	FOR p IN SELECT preds.flt AS f FROM preds ORDER BY preds.n LOOP
		s := scan_of(format('SELECT id FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC LIMIT 1000000', tbl, q, p.f));
		EXECUTE format('SELECT count(*) FROM %s WHERE body @@> %L AND (%s)', tbl, q, p.f) INTO matching;
		flt := p.f; sbs := s.sbs; candidates := s.candidates;
		RETURN NEXT;
	END LOOP;
END $$;

-- the ranker hands out the rows that satisfy the tests and no other (no unknown value, no doc in the write buffer yet); a search of a range of the
-- column reads fewer superblocks than a search of all, down to none for a range without a value
SELECT flt, sbs <= total AS within, sbs < total AS fewer, sbs = 0 AS none, candidates = matching AS exact FROM scan_gaps('common', 'docs') ORDER BY 1;
SELECT * FROM answer_diffs('common');
SELECT * FROM answer_diffs('dim_a3 AND w7');

-- rows of the write buffer, then promoted: they pass every test until their segment has a range
INSERT INTO docs SELECT 40000 + g, 'common dim_a' || g % 7 || ' w' || g % 30, g % 100, DATE '2026-08-01' + g / 100, 500000 + g, g % 4, repeat('x', 600)
FROM generate_series(1, 3000) g;
INSERT INTO docs_plain SELECT * FROM docs WHERE id > 40000 ORDER BY ctid;
SELECT * FROM answer_diffs('common');
SELECT sift.promote('docs_idx') >= 0 AND sift.promote('docs_plain_idx') >= 0 AS promoted;
SELECT * FROM answer_diffs('common');
SELECT flt, sbs <= total AS within, sbs < total AS fewer, candidates = matching AS exact FROM scan_gaps('common', 'docs') WHERE flt LIKE 'created_at >%' OR flt LIKE 'counter_a%' ORDER BY 1;

-- updated and deleted rows, VACUUM, merge
DELETE FROM docs WHERE id % 5 = 0;
DELETE FROM docs_plain WHERE id % 5 = 0;
UPDATE docs SET created_at = created_at + 100, counter_a = counter_a + 7, category_id = category_id + 1 WHERE id % 7 = 0;
UPDATE docs_plain SET created_at = created_at + 100, counter_a = counter_a + 7, category_id = category_id + 1 WHERE id % 7 = 0;
SELECT * FROM answer_diffs('common');
VACUUM docs, docs_plain;
SELECT * FROM answer_diffs('common');
SELECT sift.merge('docs_idx') >= 0 AND sift.merge('docs_plain_idx') >= 0 AS merged;
SELECT * FROM answer_diffs('common');
SELECT * FROM answer_diffs('dim_a3 AND w7');

-- a doc whose heap tuple is gone when its segment is written has no value to test: rows that are deleted and pruned (VACUUM without index cleanup) before
-- their promotion are written as unknown, and the superblock that holds them is read whatever its range
SELECT flt, sbs FROM scan_gaps('common', 'docs') WHERE flt IN ('created_at < ''2020-01-01''', 'counter_a < 0') ORDER BY 1;
INSERT INTO docs SELECT 60000 + g, 'common dim_a' || g % 7 || ' w' || g % 30, g % 100, DATE '2026-09-01' + g / 100, 600000 + g, g % 4, repeat('x', 600)
FROM generate_series(1, 300) g;
INSERT INTO docs_plain SELECT * FROM docs WHERE id > 60000 ORDER BY ctid;
DELETE FROM docs WHERE id > 60000 AND id % 10 = 0;
DELETE FROM docs_plain WHERE id > 60000 AND id % 10 = 0;
VACUUM (INDEX_CLEANUP off) docs, docs_plain;
SELECT sift.promote('docs_idx') >= 0 AND sift.promote('docs_plain_idx') >= 0 AS promoted_dead;
SELECT * FROM answer_diffs('common');
SELECT flt, sbs FROM scan_gaps('common', 'docs') WHERE flt IN ('created_at < ''2020-01-01''', 'counter_a < 0') ORDER BY 1;
SELECT sift.merge('docs_idx') >= 0 AND sift.merge('docs_plain_idx') >= 0 AS merged_dead;
SELECT * FROM answer_diffs('common');
SELECT flt, sbs FROM scan_gaps('common', 'docs') WHERE flt IN ('created_at < ''2020-01-01''', 'counter_a < 0') ORDER BY 1;

-- a REINDEX writes the ranges again
REINDEX INDEX docs_idx;
REINDEX INDEX docs_plain_idx;
SELECT flt, sbs <= total AS within, sbs < total AS fewer, sbs = 0 AS none, candidates = matching AS exact FROM scan_gaps('common', 'docs') ORDER BY 1;
SELECT * FROM answer_diffs('common');
SELECT * FROM answer_diffs('dim_a3 AND w7');

DROP FUNCTION top_of(regclass, text, text, int), answer_diffs(text), scan_of(text), scan_gaps(text, regclass);
DROP TABLE docs, docs_plain, preds;
DROP EXTENSION pg_sift;
