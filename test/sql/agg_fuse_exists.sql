-- UNION ALL arms that test an EXISTS over the elements of an array (a lookup per element of the entity's kind) share one pass over the index:
-- the test and its lookup run once for the arms together, and an arm that has a test the others lack applies it to its own set.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE dim_a (id int PRIMARY KEY, kind text);
INSERT INTO dim_a SELECT i, CASE WHEN i % 7 = 0 THEN NULL WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(0, 48) i;
CREATE TABLE mappings (kind text, category text, val text, derived_category text);
INSERT INTO mappings VALUES ('a', 'catg_l', '', 'catg_j'), ('a', 'catg_l', '', 'duplicate'), ('b', 'catg_l', '', 'catg_k'), ('b', 'catg_a', 'x', 'catg_b'),
 ('a', 'nul', NULL, 'never'), ('a', 'catg_a', 'x', NULL), (NULL, 'catg_l', '', 'catg_j');
CREATE TABLE facts (id int, dim_a_id int, day date, document jsonb) WITH (autovacuum_enabled = false);
INSERT INTO facts
SELECT i, i % 52, '2026-01-01'::date + (i % 9),
 CASE WHEN i % 29 = 0 THEN NULL
      WHEN i % 31 = 0 THEN '{}'::jsonb
      ELSE jsonb_build_object('parts', CASE WHEN i % 7 = 0 THEN '[]'::jsonb
   WHEN i % 3 = 0 THEN '[{"category":"catg_l","val":"","dir_a":"up","score":8}]'::jsonb
   ELSE '[{"category":"catg_l","val":"","dir_a":"up","score":9},{"category":"catg_a","val":"x","dir_a":"down","score":3},{"category":"zzz","val":"","dir_a":"flat","score":null},{"category":"nul","val":null,"dir_a":"flat","score":1}]'::jsonb END)
   || CASE WHEN i % 5 = 0 THEN '{"flag_a":false}'::jsonb WHEN i % 11 = 0 THEN '{"flag_a":null}'::jsonb WHEN i % 13 = 0 THEN '{"flag_a":true}'::jsonb ELSE '{}'::jsonb END END
FROM generate_series(1, 6000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, day, document, jsonb_exists(document, 'flag_a'))
 WITH (paths = 'document.parts[]{category text,val text,score float8,dir_a text}');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dim_a;
ANALYZE mappings;

-- the most arms one scan served in the plan; the rows must be those of the normal plan
CREATE FUNCTION arms_shared(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Arms In One Pass: ' THEN n := greatest(n, substr(l, strpos(l, ': ') + 2)::int); END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN d = 0 AND n > 0;
END $$;

-- the test of an arm as the condition of its WHERE: the categories its elements are mapped to
\set cats_jb '(SELECT 1 FROM LATERAL (SELECT (CASE WHEN jsonb_exists(f.document, ''flag_a'') THEN o.category ELSE m.derived_category END) AS category FROM jsonb_to_recordset(f.document -> ''parts'') AS o(category text, val text, score float8, dir_a text) LEFT JOIN LATERAL (SELECT lm.derived_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category AND lm.val = o.val AND lm.derived_category IS NOT NULL AND NOT jsonb_exists(f.document, ''flag_a'') LIMIT 1) m ON true WHERE o.score IS NOT NULL) x WHERE x.category IN (''catg_j'', ''catg_b''))'
\set cats_k '(SELECT 1 FROM LATERAL (SELECT (CASE WHEN jsonb_exists(f.document, ''flag_a'') THEN o.category ELSE m.derived_category END) AS category FROM jsonb_to_recordset(f.document -> ''parts'') AS o(category text, val text, score float8, dir_a text) LEFT JOIN LATERAL (SELECT lm.derived_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category AND lm.val = o.val AND lm.derived_category IS NOT NULL AND NOT jsonb_exists(f.document, ''flag_a'') LIMIT 1) m ON true WHERE o.score IS NOT NULL) x WHERE x.category IN (''catg_k''))'
\set from 'FROM facts f JOIN dim_a e ON e.id = f.dim_a_id WHERE '

-- three arms with the same test and keys of their own
\set same_test 'SELECT e.kind AS a, NULL::date AS b, count(*) AS n, avg(f.id) AS m ' :from 'EXISTS ' :cats_jb ' GROUP BY e.kind UNION ALL SELECT e.kind, f.day, count(*), avg(f.id) ' :from 'EXISTS ' :cats_jb ' GROUP BY e.kind, f.day UNION ALL SELECT NULL, f.day, count(*), avg(f.id) ' :from 'EXISTS ' :cats_jb ' GROUP BY f.day'
SELECT arms_shared(:'same_test') AS same_test_shared, same(:'same_test') AS same_test_same;

-- a test the other arms do not have, and other tests per arm (each applies to the rows of its own arm)
\set one_test 'SELECT e.kind AS a, NULL::date AS b, count(*) AS n, avg(f.id) AS m ' :from 'EXISTS ' :cats_jb ' GROUP BY e.kind UNION ALL SELECT e.kind, f.day, count(*), avg(f.id) ' :from 'true GROUP BY e.kind, f.day'
SELECT arms_shared(:'one_test') AS one_test_shared, same(:'one_test') AS one_test_same;
\set other_tests 'SELECT e.kind AS a, NULL::date AS b, count(*) AS n, avg(f.id) AS m ' :from 'EXISTS ' :cats_jb ' GROUP BY e.kind UNION ALL SELECT e.kind, f.day, count(*), avg(f.id) ' :from 'EXISTS ' :cats_k ' GROUP BY e.kind, f.day UNION ALL SELECT NULL, f.day, count(*), avg(f.id) ' :from 'f.id > 100 GROUP BY f.day'
SELECT arms_shared(:'other_tests') AS other_tests_shared, same(:'other_tests') AS other_tests_same;

-- an arm without keys is not a set of the pass: it runs on its own, and its rows are the same
\set no_group 'SELECT count(*) AS n, avg(f.id) AS m, NULL::text AS a ' :from 'EXISTS ' :cats_jb ' UNION ALL SELECT count(*), avg(f.id), e.kind ' :from 'EXISTS ' :cats_jb ' GROUP BY e.kind'
SELECT same(:'no_group') AS no_group_same;

DROP FUNCTION arms_shared(text), same(text);
DROP TABLE facts, dim_a, mappings;
DROP EXTENSION pg_sift;
