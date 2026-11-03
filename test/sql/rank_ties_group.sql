-- ORDER BY sift.score(ctid) DESC, id LIMIT k [OFFSET m]: the rows after the score are sorted by an Incremental Sort that reads the whole group of rows tied
-- with the k-th. The scan under it is told by the finished plan, and its first search keeps the k best hits and every hit that ties with the k-th, then the
-- scan is at its end: one search, however large the group. The rows must be those of the sort of every match, across segments, write buffers, dead rows,
-- cutoffs of the score, phrases and prefixes (whose scores the text decides: those candidates are verified as they come) and a consumer that drops hits.
CREATE EXTENSION pg_sift;
\ir test/sift_plan.sql
SELECT sift.version() IS NOT NULL AS library_loaded;

-- the score of a one-term search is a function of the length of the document, and the lengths are 3 to 8 words, so the groups are large
CREATE TABLE tg (id int PRIMARY KEY, body text, pad int) WITH (autovacuum_enabled = off);
CREATE FUNCTION tg_doc(g int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT 'tie ' || repeat('pad ', g % 6) || CASE WHEN g % 50 = 0 THEN 'rare ' ELSE '' END || CASE WHEN g % 7 = 0 THEN 'seven' ELSE 'other' END
$$;
INSERT INTO tg SELECT g, tg_doc(g), g % 9 FROM generate_series(1, 8000) g;
CREATE INDEX tg_idx ON tg USING sift (body) WITH (write_buffer_limit = 64);
-- more segments: batches that are promoted one by one, and a last one that stays in the write buffers
INSERT INTO tg SELECT g, tg_doc(g), g % 9 FROM generate_series(8001, 12000) g;
SELECT sift.promote('tg_idx') IS NOT NULL AS promoted;
INSERT INTO tg SELECT g, tg_doc(g), g % 9 FROM generate_series(12001, 14000) g;
SELECT sift.promote('tg_idx') IS NOT NULL AS promoted;
INSERT INTO tg SELECT g, tg_doc(g), g % 9 FROM generate_series(14001, 15000) g;
DELETE FROM tg WHERE id % 10 = 3;
UPDATE tg SET body = body || ' moved' WHERE id % 13 = 0;
ANALYZE tg;
SELECT count(*) > 1 AS several_segments FROM sift.segments('tg_idx') WHERE kind = 'immutable';

-- the rows of the sort of every match (one unlimited search), and the ranker searches of the scan under the sort key
CREATE FUNCTION unlimited(q text, k int, m int DEFAULT 0, extra text DEFAULT '') RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM tg WHERE body @@> %L %s) a ORDER BY s DESC, id OFFSET 0) t WHERE true', q, extra) INTO r;
	RETURN r[m + 1:m + k];
END $$;
CREATE FUNCTION limited(q text, k int, m int DEFAULT 0, extra text DEFAULT '') RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
	EXECUTE format('SELECT array_agg(id) FROM (SELECT id FROM tg WHERE body @@> %L %s ORDER BY sift.score(ctid) DESC, id LIMIT %s OFFSET %s) t', q, extra, k, m) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION scan_node(q text, k int, m int DEFAULT 0, extra text DEFAULT '') RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT id FROM tg WHERE body @@> %L %s ORDER BY sift.score(ctid) DESC, id LIMIT %s OFFSET %s', q, extra, k, m) INTO j;
	RETURN (SELECT n FROM jsonb_path_query(j, 'strict $.**') n WHERE n ->> 'Custom Plan Provider' = 'SiftScan' LIMIT 1);
END $$;

CREATE TABLE cases (q text, k int, m int);
INSERT INTO cases VALUES ('tie', 10, 0), ('tie', 100, 0), ('tie pad', 20, 0), ('seven', 10, 0), ('rare', 10, 0), ('rare seven', 5, 0), ('pad OR rare', 10, 0),
	('tie AND seven', 50, 0), ('other', 1, 0), ('tie', 1, 0), ('tie', 2000, 0), ('tie', 7, 5), ('tie', 3000, 11000), ('moved', 20, 0), ('moved tie', 15, 3), ('absent', 5, 0),
	('"tie pad"', 40, 0), ('"pad pad"', 300, 7), ('"tie pad pad" OR seven', 25, 0), ('pad*', 60, 0);

-- the answers are the sort of every match
SELECT sift_plan('SELECT id FROM tg WHERE body @@> ''tie'' ORDER BY sift.score(ctid) DESC, id LIMIT 10') LIKE '%Includes Ties: true%' AS ties_in_the_plan;
SELECT c.q, c.k, c.m FROM cases c WHERE limited(c.q, c.k, c.m) IS DISTINCT FROM unlimited(c.q, c.k, c.m) ORDER BY 1, 2, 3;

-- one ranker search whatever the group: the rows of the plan are the k best and the ones tied with the k-th (all of them for 'tie', a group of thousands)
SELECT c.q, c.k, c.m, coalesce((scan_node(c.q, c.k, c.m) -> 'Ranker Searches')::int, 1) AS searches, scan_node(c.q, c.k, c.m) ->> 'Includes Ties' AS ties
FROM cases c WHERE c.q IN ('tie', 'tie pad', 'seven', 'pad OR rare') ORDER BY 1, 2, 3;
SELECT (scan_node('tie', 10) ->> 'Limit Hint')::int AS hint_is_the_limit, (scan_node('tie', 10, 5) ->> 'Limit Hint')::int AS hint_has_the_offset;

-- a cutoff of the score and a qual of the scan's own: the plan drops hits after the ranker, which goes on with its next search
SELECT limited('tie', 30, 0, 'AND sift.score(ctid) >= 1.2') IS NOT DISTINCT FROM unlimited('tie', 30, 0, 'AND sift.score(ctid) >= 1.2') AS cutoff_same;
-- a cutoff in the middle of the groups: the hits under it are dropped by the plan, and the rows the sort wants come from the searches after the first
SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY sift.score(ctid))::float4::text AS mid FROM tg WHERE body @@> 'tie' \gset
SELECT limited('tie', 9000, 0, 'AND sift.score(ctid) >= ' || :'mid') IS NOT DISTINCT FROM unlimited('tie', 9000, 0, 'AND sift.score(ctid) >= ' || :'mid') AS mid_cutoff_same;
SELECT coalesce((scan_node('tie', 9000, 0, 'AND sift.score(ctid) >= ' || :'mid') -> 'Ranker Searches')::int, 1) AS mid_cutoff_searches, scan_node('tie', 9000, 0, 'AND sift.score(ctid) >= ' || :'mid') ->> 'Includes Ties' AS mid_cutoff_ties;
SELECT limited('tie', 30, 0, 'AND pad < 4') IS NOT DISTINCT FROM unlimited('tie', 30, 0, 'AND pad < 4') AS qual_same;
SELECT limited('tie', 2000, 0, 'AND pad = 3') IS NOT DISTINCT FROM unlimited('tie', 2000, 0, 'AND pad = 3') AS selective_qual_same;

-- a scan that is not under an incremental sort under a limit is not told: a full sort, WITH TIES, a limit that is a parameter
SELECT sift_plan('SELECT id FROM tg WHERE body @@> ''tie'' ORDER BY sift.score(ctid) DESC, id') LIKE '%Includes Ties%' AS plain_sort;
SELECT sift_plan('SELECT id FROM tg WHERE body @@> ''tie'' ORDER BY sift.score(ctid) DESC, id FETCH FIRST 10 ROWS WITH TIES') LIKE '%Includes Ties%' AS with_ties;
CREATE FUNCTION tg_top(n int) RETURNS int[] LANGUAGE plpgsql SET plan_cache_mode = force_generic_plan AS $$
BEGIN
	RETURN (SELECT array_agg(id) FROM (SELECT id FROM tg WHERE body @@> 'tie' ORDER BY sift.score(ctid) DESC, id LIMIT n) t);
END $$;
SELECT tg_top(25) = unlimited('tie', 25) AS parameter_limit_same;

-- the same on rows that were changed under an open snapshot: the tied group is of the rows the snapshot sees
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS seen FROM tg;
DELETE FROM tg WHERE id % 2 = 0 AND id > 14000;
SELECT limited('tie', 500, 0) IS NOT DISTINCT FROM unlimited('tie', 500, 0) AS own_changes_same;
ROLLBACK;

DROP FUNCTION tg_top(int), unlimited(text, int, int, text), limited(text, int, int, text), scan_node(text, int, int, text), sift_plan(text), tg_doc(int);
DROP TABLE tg, cases;
DROP EXTENSION pg_sift;
