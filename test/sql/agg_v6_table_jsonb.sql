-- SiftAggV6 over a jsonb column of a sift_table: the `->>` text of a key as a group key, a DISTINCT argument, a cast aggregate argument and a qual is answered like the heap (sift.enable_agg = off);
-- the first word says the answers agree, the second whether the plan is the node's
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE TABLE tj (id int PRIMARY KEY, g int, doc jsonb) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO tj
SELECT i, i % 7, jsonb_build_object('k', 'k' || i % 5, 'v', i % 13, 'f', i * 0.5, 'o', CASE WHEN i % 11 = 0 THEN NULL ELSE to_jsonb(i % 3) END)
FROM generate_series(1, 40000) i;
INSERT INTO tj SELECT 40000 + i, i % 7, NULL FROM generate_series(1, 30) i;
INSERT INTO tj SELECT 41000 + i, i % 7, '{"v": "7", "k": null}'::jsonb FROM generate_series(1, 30) i;
VACUUM ANALYZE tj;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; l text; plan text := '';
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6%' THEN 'node' ELSE 'normal' END) || ' ' || q;
END $$;

SELECT same(q) FROM unnest(ARRAY[
	'SELECT doc ->> ''k'', count(*) FROM tj GROUP BY 1',
	'SELECT sum((doc ->> ''v'')::int) FROM tj',
	'SELECT g, sum((doc ->> ''v'')::int), count(doc ->> ''k'') FROM tj GROUP BY g',
	'SELECT count(*) FROM tj WHERE doc ->> ''k'' = ''k3''',
	'SELECT doc ->> ''k'', sum((doc ->> ''f'')::float8) FROM tj WHERE doc ->> ''k'' IN (''k1'', ''k2'') GROUP BY 1',
	'SELECT doc ->> ''o'', count(*) FROM tj WHERE g = 2 GROUP BY 1',
	'SELECT count(*) FROM tj WHERE doc ? ''o''',
	'SELECT g, count(DISTINCT doc ->> ''k'') FROM tj GROUP BY g',
	'SELECT count(DISTINCT doc -> ''o'') FROM tj',
	'SELECT g, max(doc ->> ''k''), min(doc ->> ''k'') FROM tj GROUP BY g'
]) AS q;

DROP FUNCTION same(text);
DROP TABLE tj;
DROP EXTENSION pg_sift;
