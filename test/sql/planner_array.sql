-- @@> against an array (ANY, ALL, NOT ANY, prepared) plans and answers like the SQL it stands for: ANY is one scan of the OR of its queries
-- (a SiftScan, a SiftCount, the score), the bitmap scan with one index scan per element stays as sift.enable_scan = off; array_any_score
-- tests the scores and the ranking
CREATE EXTENSION pg_sift;

CREATE TABLE a (id int, body text);
INSERT INTO a SELECT i, 'alpha beta ' || i || CASE WHEN i % 5 = 0 THEN ' gamma' ELSE '' END FROM generate_series(1, 2000) i;
CREATE INDEX a_idx ON a USING sift (body);
VACUUM ANALYZE a;
SET enable_seqscan = off;

EXPLAIN (COSTS OFF) SELECT id FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']);
EXPLAIN (COSTS OFF) SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']);
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) ORDER BY 2 DESC LIMIT 3;
-- planning only: ALL and NOT ANY are not index quals, their plan depends on the server version
DO $$ BEGIN
	EXECUTE 'EXPLAIN SELECT id FROM a WHERE body @@> ALL (ARRAY[''7'', ''gamma''])';
	EXECUTE 'EXPLAIN SELECT id FROM a WHERE NOT body @@> ANY (ARRAY[''7'', ''gamma''])';
END $$;

SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']);
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) AND body @@> 'beta';
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['nomatch']);
SELECT count(*) FROM a WHERE body @@> ANY ('{}'::text[]);
SELECT count(*) FROM a WHERE body @@> ALL (ARRAY['7', 'gamma']);
SELECT count(*) FROM a WHERE body @@> ALL (ARRAY['10', 'gamma']);
SELECT count(*) FROM a WHERE NOT body @@> ANY (ARRAY['7', 'gamma']);
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) AND body @@> '10';

-- the same counts through the bitmap scan (one index scan per element) and through a sequential scan
SET sift.enable_scan = off;
SET sift.enable_count = off;
EXPLAIN (COSTS OFF) SELECT id FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']);
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']);
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) AND body @@> 'beta';
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['nomatch']);
SELECT count(*) FROM a WHERE body @@> ANY ('{}'::text[]);
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) AND body @@> '10';
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_seqscan = on;
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']);
SELECT count(*) FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) AND body @@> 'beta';
SELECT count(*) FROM a WHERE NOT body @@> ANY (ARRAY['7', 'gamma']);
RESET enable_indexscan;
RESET enable_bitmapscan;
SET enable_seqscan = off;

PREPARE any_p(text[]) AS SELECT count(*) FROM a WHERE body @@> ANY ($1);
EXECUTE any_p(ARRAY['7', 'gamma']);
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE any_p(ARRAY['7', 'gamma']);
EXECUTE any_p(ARRAY['7', 'gamma']);
EXECUTE any_p(ARRAY['nomatch']);
RESET sift.enable_scan;
RESET sift.enable_count;
DEALLOCATE any_p;
PREPARE any_p(text[]) AS SELECT count(*) FROM a WHERE body @@> ANY ($1);
EXPLAIN (COSTS OFF) EXECUTE any_p(ARRAY['7', 'gamma']);
EXECUTE any_p(ARRAY['7', 'gamma']);
EXECUTE any_p(ARRAY['nomatch']);
RESET plan_cache_mode;

-- the score comes from the scan of the OR; with the bitmap scan there is none
SELECT id FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) ORDER BY sift.score(ctid) DESC LIMIT 3;
SET sift.enable_scan = off;
SELECT id FROM a WHERE body @@> ANY (ARRAY['7', 'gamma']) ORDER BY sift.score(ctid) DESC LIMIT 3;
RESET sift.enable_scan;

DROP TABLE a;
DROP EXTENSION pg_sift;
