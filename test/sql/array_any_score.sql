-- body @@> ANY (array) is the OR of its queries: the scan answers it as one query, with the BM25 scores and the ranking of the
-- same OR written in one query text
CREATE EXTENSION pg_sift;

CREATE TABLE t (id int, body text);
INSERT INTO t
SELECT i, 'alpha ' || (ARRAY['beta', 'gamma', 'delta', 'epsilon', 'zeta'])[1 + i % 5] || ' w' || i % 11
	|| CASE WHEN i % 7 = 0 THEN ' pool' WHEN i % 13 = 0 THEN ' spa spa' ELSE '' END
	|| CASE WHEN i % 17 = 0 THEN ' pool spa' ELSE '' END
FROM generate_series(1, 3000) i;
CREATE INDEX t_idx ON t USING sift (body);
VACUUM ANALYZE t;
SET enable_seqscan = off;

-- the scored plan, top-N, the count and the match list
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) ORDER BY 2 DESC LIMIT 5;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);
EXPLAIN (COSTS OFF) SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);

-- the same match set as the bitmap scan and as the OR text
CREATE TABLE ref AS SELECT id FROM t WHERE body @@> 'pool OR spa';
SET sift.enable_scan = off;
SET sift.enable_count = off;
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);
SELECT count(*) FROM (SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) EXCEPT SELECT id FROM ref) d;
SELECT count(*) FROM (SELECT id FROM ref EXCEPT SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa'])) d;
RESET sift.enable_scan;
RESET sift.enable_count;
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);
SELECT count(*) FROM (SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) EXCEPT SELECT id FROM ref) d;
SELECT count(*) FROM (SELECT id FROM ref EXCEPT SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa'])) d;

-- the scores and the ranking are those of the OR text, for every array
CREATE FUNCTION score_same(arr text[], txt text, OUT matches int, OUT same boolean) LANGUAGE plpgsql AS $$
DECLARE a text[]; b text[];
BEGIN
	EXECUTE 'SELECT array_agg(id || '':'' || s ORDER BY id) FROM (SELECT id, sift.score(ctid) s FROM t WHERE body @@> ANY ($1)) x' INTO a USING arr;
	EXECUTE 'SELECT array_agg(id || '':'' || s ORDER BY id) FROM (SELECT id, sift.score(ctid) s FROM t WHERE body @@> $1) x' INTO b USING txt;
	matches := cardinality(a);
	same := a IS NOT DISTINCT FROM b;
END $$;
CREATE FUNCTION rank_same(arr text[], txt text, k int) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a int[]; b int[];
BEGIN
	EXECUTE 'SELECT array_agg(id) FROM (SELECT id FROM t WHERE body @@> ANY ($1) ORDER BY sift.score(ctid) DESC, id LIMIT $3) x' INTO a USING arr, txt, k;
	EXECUTE 'SELECT array_agg(id) FROM (SELECT id FROM t WHERE body @@> $2 ORDER BY sift.score(ctid) DESC, id LIMIT $3) x' INTO b USING arr, txt, k;
	RETURN a IS NOT DISTINCT FROM b AND a IS NOT NULL;
END $$;
SELECT * FROM score_same(ARRAY['pool', 'spa'], 'pool OR spa');
SELECT * FROM score_same(ARRAY['pool', 'spa', 'gamma'], 'pool OR spa OR gamma');
SELECT * FROM score_same(ARRAY['pool'], 'pool');
SELECT * FROM score_same(ARRAY['pool', 'pool'], 'pool OR pool');
SELECT * FROM score_same(ARRAY['pool spa', 'zeta'], '(pool spa) OR zeta');
SELECT * FROM score_same(ARRAY['"alpha gamma"', 'spa^3'], '"alpha gamma" OR spa^3');
SELECT * FROM score_same(ARRAY['alp*', 'w1*'], 'alp* OR w1*');
SELECT * FROM score_same(ARRAY['gamma NOT w3', 'pool OR spa'], '(gamma NOT w3) OR pool OR spa');
SELECT * FROM score_same(ARRAY['zeta', NULL, 'spa'], 'zeta OR spa');
SELECT rank_same(ARRAY['pool', 'spa'], 'pool OR spa', 5);
SELECT rank_same(ARRAY['pool', 'spa'], 'pool OR spa', 100);
SELECT rank_same(ARRAY['pool spa', 'zeta', 'w4'], '(pool spa) OR zeta OR w4', 20);
SELECT id, round(sift.score(ctid)::numeric, 4) AS score FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) ORDER BY sift.score(ctid) DESC, id LIMIT 5;
SELECT id, round(sift.score(ctid)::numeric, 4) AS score FROM t WHERE body @@> 'pool OR spa' ORDER BY sift.score(ctid) DESC, id LIMIT 5;

-- filters, a second query, a cutoff and the max_score
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) AND id % 3 = 0;
SELECT count(*) FROM t WHERE body @@> 'pool OR spa' AND id % 3 = 0;
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) AND body @@> 'gamma';
SELECT count(*) FROM t WHERE body @@> 'pool OR spa' AND body @@> 'gamma';
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) AND body @@> ANY (ARRAY['gamma', 'zeta']);
SELECT count(*) FROM t WHERE body @@> '(pool OR spa) AND (gamma OR zeta)';
SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) AND id % 3 = 0 ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT id FROM t WHERE body @@> 'pool OR spa' AND id % 3 = 0 ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) AND sift.score(ctid) > 3;
SELECT count(*) FROM t WHERE body @@> 'pool OR spa' AND sift.score(ctid) > 3;
SELECT round(max(sift.max_score(ctid))::numeric, 4) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);
SELECT round(max(sift.max_score(ctid))::numeric, 4) FROM t WHERE body @@> 'pool OR spa';

-- NULL and empty arrays, NULL elements, a nonexistent word
SELECT count(*) FROM t WHERE body @@> ANY (NULL::text[]);
SELECT count(*) FROM t WHERE body @@> ANY ('{}'::text[]);
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY[NULL]::text[]);
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['nomatch']);
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['nomatch', NULL, 'spa']);
SELECT count(*) FROM t WHERE body @@> 'spa';
SELECT id FROM t WHERE body @@> ANY (NULL::text[]) ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id FROM t WHERE body @@> ANY ('{}'::text[]) ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id FROM t WHERE body @@> ANY (ARRAY[NULL, 'nomatch']) ORDER BY sift.score(ctid) DESC LIMIT 3;

-- an element is a whole query: an empty or a malformed one is the error of the same query on its own
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', '']);
SELECT count(*) FROM t WHERE body @@> '';
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa AND']);
SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', '(spa']) ORDER BY sift.score(ctid) DESC LIMIT 3;

-- ALL and NOT ANY are not an OR of matches: no scan answers them, and they have no score
DO $$ BEGIN
	EXECUTE 'EXPLAIN SELECT id FROM t WHERE body @@> ALL (ARRAY[''pool'', ''spa''])';
	EXECUTE 'EXPLAIN SELECT id FROM t WHERE NOT body @@> ANY (ARRAY[''pool'', ''spa''])';
END $$;
SELECT count(*) FROM t WHERE body @@> ALL (ARRAY['pool', 'spa']);
SELECT count(*) FROM t WHERE body @@> 'pool AND spa';
SELECT count(*) FROM t WHERE NOT body @@> ANY (ARRAY['pool', 'spa']);
SELECT count(*) FROM t WHERE NOT body @@> 'pool OR spa';
SELECT id FROM t WHERE body @@> ALL (ARRAY['pool', 'spa']) ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id FROM t WHERE NOT body @@> ANY (ARRAY['pool', 'spa']) ORDER BY sift.score(ctid) DESC LIMIT 3;

-- a parameter: the array of a generic plan is read when the scan starts
PREPARE any_s(text[]) AS SELECT id, round(sift.score(ctid)::numeric, 4) FROM t WHERE body @@> ANY ($1) ORDER BY sift.score(ctid) DESC, id LIMIT 3;
PREPARE any_c(text[]) AS SELECT count(*) FROM t WHERE body @@> ANY ($1);
SET plan_cache_mode = force_generic_plan;
EXECUTE any_s(ARRAY['pool', 'spa']);
EXECUTE any_s(ARRAY['zeta']);
EXECUTE any_s(NULL);
EXECUTE any_s('{}');
EXECUTE any_c(ARRAY['pool', 'spa']);
EXECUTE any_c(ARRAY['pool', NULL]);
EXECUTE any_c(NULL);
RESET plan_cache_mode;
EXECUTE any_s(ARRAY['pool', 'spa']);
EXECUTE any_c(ARRAY['pool', 'spa']);

-- rows written after the index was built (the write buffer) are matched and scored too
INSERT INTO t VALUES (100001, 'alpha pool pool spa'), (100002, 'alpha zzz');
SELECT id FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']) ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT id FROM t WHERE body @@> 'pool OR spa' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'spa']);
SELECT count(*) FROM t WHERE body @@> 'pool OR spa';
SELECT count(*) FROM t WHERE body @@> ANY (ARRAY['pool', 'zzz']);

DROP TABLE ref;
DROP TABLE t;
DROP FUNCTION score_same(text[], text);
DROP FUNCTION rank_same(text[], text, int);
DROP EXTENSION pg_sift;
