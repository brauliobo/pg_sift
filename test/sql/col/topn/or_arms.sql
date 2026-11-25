-- Top-N of a BM25 match whose WHERE ORs searches of the index next to other conditions: a plain match ANDed with an OR (a AND (b OR c)), two ORs, an OR with
-- column filters. The match set is the union of the arms, each the plain matches and one branch of every OR. Every statement runs with the pushdown and
-- with sift.enable_agg = off and the rows are compared in order.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;

CREATE TABLE oa (id serial PRIMARY KEY, store int, status text, ts timestamp, memo text);
INSERT INTO oa (store, status, ts, memo)
SELECT i % 40, (ARRAY['paid', 'shipped', 'x'])[1 + i % 3], timestamp '2024-01-01' + i * interval '97 seconds',
	CASE WHEN i % 41 = 0 THEN NULL ELSE (ARRAY['gift wire cable', 'fast cable', 'wire only', 'gift box', 'red blue', 'cable wire gift', 'express size', 'blue wire'])[1 + i % 8] || ' w' || (i % 50) END
FROM generate_series(1, 20000) i;
SET client_min_messages = warning;
CREATE INDEX oa_bm ON oa USING sift (memo);
RESET client_min_messages;
CREATE INDEX oa_col ON oa USING sift_col (id, store, status, ts) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) oa;

CREATE FUNCTION oa_check(q text, workers int DEFAULT 0) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text;
BEGIN
	DROP TABLE IF EXISTS oa_a, oa_b;
	PERFORM sift_helpers.agg_mode_workers(true, workers);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE oa_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM sift_helpers.agg_mode_workers(false, 0);
	EXECUTE 'CREATE TEMP TABLE oa_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM oa_a EXCEPT ALL SELECT pos, r FROM oa_b) UNION ALL (SELECT pos, r FROM oa_b EXCEPT ALL SELECT pos, r FROM oa_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || ' ' || (SELECT count(*) FROM oa_b);
END $$;

SET seq_page_cost = 10000;
SET random_page_cost = 10000;

SELECT oa_check(format('SELECT id, ts FROM oa WHERE %s ORDER BY ts DESC, id LIMIT 10', w)) AS r, w
FROM unnest(ARRAY[
	'memo @@> ''cable'' OR memo @@> ''wire''',
	'memo @@> ''gift'' AND (memo @@> ''cable'' OR memo @@> ''box'')',
	'(memo @@> ''cable'' OR memo @@> ''box'') AND memo @@> ''gift''',
	'(memo @@> ''cable'' OR memo @@> ''wire'') AND (memo @@> ''gift'' OR memo @@> ''blue'')',
	'(memo @@> ''cable'' OR memo @@> ''wire'') AND (memo @@> ''gift'' OR memo @@> ''blue'') AND memo @@> ''w7''',
	'(memo @@> ''cable'' OR memo @@> ''wire'' OR memo @@> ''box'') AND status = ''paid'' AND store < 20',
	'memo @@> ''gift'' AND (memo @@> ''cable'' OR memo @@> ''box'') AND store <> 3',
	'memo @@> ''express'' AND (memo @@> ''cable'' OR memo @@> ''wire'')',
	'memo @@> ''"wire cable"'' OR memo @@> ''"gift box"''',
	'memo @@> ''gift'' AND (memo @@> ''"wire cable"'' OR memo @@> ''red'')',
	'memo @@> ''cable'' AND (memo @@> ''cable'' OR memo @@> ''wire'')']) w;

-- an offset, an ascending order, parallel workers, a filter that leaves nothing
SELECT oa_check('SELECT id, ts FROM oa WHERE memo @@> ''gift'' AND (memo @@> ''cable'' OR memo @@> ''box'') ORDER BY ts, id LIMIT 12 OFFSET 40');
SELECT oa_check('SELECT id, store FROM oa WHERE (memo @@> ''cable'' OR memo @@> ''wire'') AND (memo @@> ''gift'' OR memo @@> ''blue'') ORDER BY store DESC, id LIMIT 300', 2);
SELECT oa_check('SELECT id FROM oa WHERE memo @@> ''gift'' AND (memo @@> ''cable'' OR memo @@> ''box'') AND store > 1000 ORDER BY ts, id LIMIT 5');

-- an OR with a predicate on a column is not an arm of the match set: the BM25 index scan answers it, not the top-N
EXPLAIN (COSTS OFF) SELECT id FROM oa WHERE memo @@> 'gift' AND (memo @@> 'cable' OR store = 3) ORDER BY ts, id LIMIT 5;

DROP TABLE oa;
DROP FUNCTION oa_check(text, int);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
