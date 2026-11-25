-- The null test and the count of the text of a member of the elements of jsonb_array_elements (`p ->> 'score' IS NOT NULL`, `count(p ->> 'score')`) of a
-- list whose index declares the member typed (`items[]{score float8}`) are those of the typed leaf: `->>` is NULL for a missing key and for a JSON null,
-- the leaf too. Every query is run with the scan off and on and the answers compared; a member that holds a value the leaf cannot store keeps the
-- answer of the normal plan (the plan or the scan's error, whichever it is, is that of the cast read of the same member).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
\ir test/agg_helpers.sql

CREATE TABLE ed (id int PRIMARY KEY, g int, doc jsonb);
INSERT INTO ed
SELECT i, i % 7,
       CASE WHEN i % 17 = 0 THEN jsonb_build_object('items', '[]'::jsonb)
            WHEN i % 19 = 0 THEN '{}'::jsonb
            ELSE jsonb_build_object('items', (SELECT jsonb_agg(
                   CASE (i + j) % 5
                     WHEN 0 THEN jsonb_build_object('tag', 't' || j)
                     WHEN 1 THEN jsonb_build_object('tag', 't' || j, 'score', NULL)
                     WHEN 2 THEN jsonb_build_object('tag', 't' || j, 'score', (i * j) % 11 - 5)
                     WHEN 3 THEN jsonb_build_object('tag', 't' || j, 'score', ((i * j) % 100) / 8.0)
                     ELSE jsonb_build_object('tag', 'u', 'score', 0, 'n', j)
                   END) FROM generate_series(1, 1 + i % 6) j)) END
FROM generate_series(1, 4000) i;
CREATE INDEX ed_col ON ed USING sift_col (id, g, doc) WITH (paths = 'doc.items[]{tag text, score float8, n int4}');
VACUUM (FREEZE, ANALYZE) ed;

CREATE FUNCTION check_query(label text, q text) RETURNS TABLE (what text, in_scan bool, same bool, answer text) LANGUAGE sql AS $$
	SELECT label, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 60)
$$;
SELECT * FROM check_query('not null', $q$ SELECT count(*) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'score' IS NOT NULL $q$);
SELECT * FROM check_query('null', $q$ SELECT g, count(*) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'score' IS NULL GROUP BY g $q$);
SELECT * FROM check_query('not (x is null)', $q$ SELECT g, count(*) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE NOT (p ->> 'score' IS NULL) GROUP BY g $q$);
SELECT * FROM check_query('count of the text', $q$ SELECT count(p ->> 'score') AS a, count(*) AS b, count(p ->> 'tag') AS c FROM ed, jsonb_array_elements(doc -> 'items') p $q$);
SELECT * FROM check_query('count of the text, grouped', $q$ SELECT g, count(p ->> 'score') AS a, count(p ->> 'n') AS b FROM ed, jsonb_array_elements(doc -> 'items') p GROUP BY g $q$);
SELECT * FROM check_query('count and avg of the filtered', $q$ SELECT count(p ->> 'score'), avg((p ->> 'score')::float8) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'score' IS NOT NULL $q$);
SELECT * FROM check_query('count with a filter', $q$ SELECT count(p ->> 'score') FILTER (WHERE p ->> 'tag' = 'u') AS a, count(p ->> 'n') FILTER (WHERE p ->> 'score' IS NOT NULL) AS b FROM ed, jsonb_array_elements(doc -> 'items') p $q$);
SELECT * FROM check_query('a case', $q$ SELECT sum(CASE WHEN p ->> 'score' IS NULL THEN 1 ELSE 0 END) AS a, sum(CASE WHEN p ->> 'n' IS NOT NULL THEN 1 ELSE 0 END) AS b FROM ed, jsonb_array_elements(doc -> 'items') p $q$);
SELECT * FROM check_query('or with a comparison', $q$ SELECT count(*) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'score' IS NULL OR (p ->> 'score')::float8 > 5 $q$);
SELECT * FROM check_query('the integer member', $q$ SELECT count(*) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'n' IS NOT NULL $q$);
SELECT * FROM check_query('count distinct stays', $q$ SELECT count(DISTINCT p ->> 'score') FROM ed, jsonb_array_elements(doc -> 'items') p $q$);
SELECT * FROM check_query('the text member itself', $q$ SELECT count(*) FROM ed, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'tag' IS NOT NULL $q$);
SELECT * FROM check_query('an unknown member', $q$ SELECT count(p ->> 'other') FROM ed, jsonb_array_elements(doc -> 'items') p $q$);
-- the null test of a member of the document itself reads the same leaf kinds as before
SELECT * FROM check_query('document, not null', $q$ SELECT count(*) FROM ed WHERE doc -> 'items' ->> 0 IS NOT NULL $q$);

-- a value the leaf cannot store (a string in the float8 member): the answers are those of the normal plan
CREATE TABLE ed2 (id int PRIMARY KEY, doc jsonb);
INSERT INTO ed2 SELECT i, jsonb_build_object('items', jsonb_build_array(jsonb_build_object('score', i), jsonb_build_object('score', CASE WHEN i % 50 = 0 THEN 'n/a' ELSE NULL END)))
FROM generate_series(1, 400) i;
CREATE INDEX ed2_col ON ed2 USING sift_col (id, doc) WITH (paths = 'doc.items[]{score float8}');
VACUUM (FREEZE, ANALYZE) ed2;
SELECT * FROM check_query('dirty, not null', $q$ SELECT count(*) FROM ed2, jsonb_array_elements(doc -> 'items') p WHERE p ->> 'score' IS NOT NULL $q$);
SELECT * FROM check_query('dirty, count', $q$ SELECT count(p ->> 'score') FROM ed2, jsonb_array_elements(doc -> 'items') p $q$);

DROP FUNCTION check_query(text, text), runs_in_scan(text), outcome(text, bool), pushed, runq;
DROP TABLE ed, ed2;
DROP EXTENSION pg_sift;
