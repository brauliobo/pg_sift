-- Segment format 4: the position stream of a term is coded in blocks of 32 postings. The same corpus indexed in
-- formats 3 and 4 answers every phrase alike and like the heap, the index of format 4 is smaller, and the blocks survive deletes, VACUUM,
-- merges with a segment of format 3 and terms staged in temporary files. The terms hold what a block codes: one occurrence at every width of
-- first position, runs of adjacent occurrences (Rice parameter 0), gaps of thousands among adjacent ones (unary codes of several words), and counts
-- from 2 to 60.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE pb AS
SELECT g AS id,
	CASE g % 7
		WHEN 0 THEN repeat('f ', g % 300) || 'rep'
		WHEN 1 THEN 'rep rep rep rep ' || 'x' || g % 13
		WHEN 2 THEN 'rep' || repeat(' f', 39) || ' rep' || repeat(' f', 39) || ' rep' || repeat(' f', 39) || ' rep'
		WHEN 3 THEN CASE WHEN g % 700 = 3 THEN repeat('rep ', 30) || repeat('f ', 3000) || 'rep' ELSE repeat('rep ', 1 + g % 60) END
		WHEN 4 THEN 'f f f'
		WHEN 5 THEN 'rep x' || g % 5 || ' y' || g % 3
		ELSE 'rep x rep y rep'
	END AS body
FROM generate_series(1, 6000) g;
CREATE TABLE pb4 AS SELECT * FROM pb ORDER BY id;
CREATE TABLE pb3 AS SELECT * FROM pb ORDER BY id;
CREATE TABLE pbo AS SELECT * FROM pb ORDER BY id;
SET sift.grouped_min_df = 200;
SET sift.test_segfmt = 3;
CREATE INDEX pb3_idx ON pb3 USING sift (body);
SET sift.test_segfmt = 4;
CREATE INDEX pb4_idx ON pb4 USING sift (body);
CREATE INDEX pbo_idx ON pbo USING sift (body) WITH (positions = off);

SELECT 'pb3_idx' AS idx, format FROM sift._test_segment_formats('pb3_idx') WHERE kind = 'immutable'
UNION ALL SELECT 'pb4_idx', format FROM sift._test_segment_formats('pb4_idx') WHERE kind = 'immutable'
UNION ALL SELECT 'pb4_idx metapage minor', format FROM sift._test_segment_formats('pb4_idx') WHERE kind = 'metapage minor';
SELECT sift.verify('pb3_idx', true), sift.verify('pb4_idx', true);
SELECT pg_relation_size('pb4_idx') < pg_relation_size('pb3_idx') AS blocks_smaller;

CREATE FUNCTION qs() RETURNS SETOF text LANGUAGE sql AS $$ SELECT * FROM unnest(ARRAY[
	'"rep"', '"rep rep"', '"rep rep rep rep"', '"rep f"', '"f rep"', '"f f f rep"', '"rep f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f f rep"',
	'"rep x rep"', '"rep x1"', '"rep y2"', '"x3 rep"', '"rep rep f"', '"f rep rep"', '"rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep rep f"',
	'"rep rep" AND "rep f"', '"x rep" OR "f f"']) $$;
CREATE FUNCTION cnt(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE body @@> %L', tbl, q) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION top(tbl text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id || ''='' || round(s::numeric, 6), '','' ORDER BY s DESC, id) FROM
		(SELECT id, sift.score(ctid) s FROM %I WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT 300) t', tbl, q) INTO r;
	RETURN r;
END $$;
-- the queries whose counts or ranked results differ between the two formats or from the index without positions (the heap recheck)
CREATE FUNCTION diffs() RETURNS TABLE (q text, f3 bigint, f4 bigint, heap bigint) LANGUAGE sql AS $$
	SELECT q, cnt('pb3', q), cnt('pb4', q), cnt('pbo', q) FROM qs() q
	WHERE cnt('pb3', q) <> cnt('pbo', q) OR cnt('pb4', q) <> cnt('pbo', q)
		OR top('pb3', q) IS DISTINCT FROM top('pbo', q) OR top('pb4', q) IS DISTINCT FROM top('pbo', q)
$$;
SELECT * FROM diffs();
SELECT count(*) AS queries, count(*) FILTER (WHERE cnt('pb4', q) > 0) AS with_hits, sum(cnt('pb4', q)) AS hits FROM qs() q;

-- deletes and VACUUM, then a merge: the records of the live postings are rewritten, the dead ones left out
DELETE FROM pb3 WHERE id % 5 = 1;
DELETE FROM pb4 WHERE id % 5 = 1;
DELETE FROM pbo WHERE id % 5 = 1;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pb3;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pb4;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pbo;
SELECT * FROM diffs();
SELECT sift.verify('pb4_idx', true);
SET sift.async_maintenance = off;
INSERT INTO pb3 SELECT id + 10000, body FROM pb WHERE id % 3 = 0;
INSERT INTO pb4 SELECT id + 10000, body FROM pb WHERE id % 3 = 0;
INSERT INTO pbo SELECT id + 10000, body FROM pb WHERE id % 3 = 0;
SELECT sift.promote('pb4_idx') > 0 AS promoted;
SELECT sift.merge('pb4_idx', true) > 0 AS merged;
SELECT * FROM diffs();
SELECT sift.verify('pb3_idx', true), sift.verify('pb4_idx', true);

-- a segment of format 3 and one of format 4 merged into one of format 4
SET sift.test_segfmt = 3;
CREATE TABLE pm AS SELECT * FROM pb WHERE id <= 3000;
CREATE TABLE pmo AS SELECT * FROM pb WHERE id <= 3000;
CREATE INDEX pm_idx ON pm USING sift (body);
CREATE INDEX pmo_idx ON pmo USING sift (body) WITH (positions = off);
SET sift.test_segfmt = 4;
INSERT INTO pm SELECT * FROM pb WHERE id > 3000;
INSERT INTO pmo SELECT * FROM pb WHERE id > 3000;
SELECT sift.promote('pm_idx') > 0 AS promoted;
SELECT format, count(*) AS segments FROM sift._test_segment_formats('pm_idx') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT sift.merge('pm_idx', true) > 0 AS merged;
SELECT format, count(*) AS segments FROM sift._test_segment_formats('pm_idx') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT sift.verify('pm_idx', true);
SELECT count(*) AS queries, count(*) FILTER (WHERE cnt('pm', q) = cnt('pmo', q) AND top('pm', q) IS NOT DISTINCT FROM top('pmo', q)) AS same FROM qs() q;

-- terms staged in temporary files while a segment is written
SET sift.test_segment_postings = 1000;
CREATE TABLE ps AS SELECT * FROM pb ORDER BY id;
CREATE TABLE pso AS SELECT * FROM pb ORDER BY id;
CREATE INDEX ps_idx ON ps USING sift (body);
CREATE INDEX pso_idx ON pso USING sift (body) WITH (positions = off);
SELECT sift.verify('ps_idx', true);
SELECT count(*) AS queries, count(*) FILTER (WHERE cnt('ps', q) = cnt('pso', q) AND top('ps', q) IS NOT DISTINCT FROM top('pso', q)) AS same FROM qs() q;

DROP FUNCTION diffs(), qs(), cnt(text, text), top(text, text);
DROP TABLE pb, pb3, pb4, pbo, pm, pmo, ps, pso;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
