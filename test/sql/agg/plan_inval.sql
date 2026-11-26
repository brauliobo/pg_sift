-- The relcache entry of a sift_col index (and the store of the relation) is invalidated while SiftAggV6 plans a statement over it, as a hot standby does when it replays a REINDEX: the cached state of the entry is overwritten
-- and the entry is reloaded after the planner took its schema. The plan reads the schema it took, so every answer is the heap twin's and no backend crashes.
-- requires: build col
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE ih (id int, g int, v int, tag text, at timestamptz, n numeric(12,2), attributes jsonb, metrics jsonb);
INSERT INTO ih SELECT i, i % 7, i % 13, 'tag' || i % 5, timestamptz '2030-01-01' + i * interval '1 minute', (i % 997) / 8.0,
	jsonb_build_object('channel', CASE i % 3 WHEN 0 THEN 'web' WHEN 1 THEN 'pos' ELSE 'app' END),
-- 60000 rows are 8 granules of 8192: the plan prices a scan of more than one granule while the entry is invalidated
	CASE i % 10 WHEN 0 THEN NULL WHEN 1 THEN '{}' ELSE jsonb_build_object('m_total', i * 0.5, 'quality', i % 6) END FROM generate_series(1, 60000) i;
CREATE TABLE ic (LIKE ih);
INSERT INTO ic SELECT * FROM ih;
CREATE INDEX ic_col ON ic USING sift_col (g, v, tag, at, n, attributes, metrics) WITH (paths = 'attributes->channel text, metrics.* float8');
CREATE TABLE it (id int, g int, v int, tag text, at timestamptz, n numeric(12,2)) USING sift_table;
INSERT INTO it SELECT id, g, v, tag, at, n FROM ih;
ANALYZE ih; ANALYZE ic; ANALYZE it;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text; engine text := 'native';
BEGIN
	FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF plan LIKE '%SiftAggV6%' THEN engine := 'v6'; END IF;
	END LOOP;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO a;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || regexp_replace(regexp_replace(q, '\mic\M', 'ih', 'g'), '\mit\M', 'ih', 'g') || ') x' INTO b;
	RETURN engine || ' ' || CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;

CREATE FUNCTION checks(rel text, with_json boolean) RETURNS TABLE (shape text, answer text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT 'grouped', same(format('SELECT g, count(*), sum(v) FROM %s GROUP BY g', rel));
	RETURN QUERY SELECT 'text key', same(format('SELECT tag, count(*), min(v), max(v) FROM %s WHERE g < 5 GROUP BY tag', rel));
	RETURN QUERY SELECT 'filtered', same(format('SELECT g, count(*) FROM %s WHERE v BETWEEN 3 AND 9 AND at >= timestamptz ''2030-01-10'' GROUP BY g', rel));
	RETURN QUERY SELECT 'distinct', same(format('SELECT g, count(DISTINCT v) FROM %s GROUP BY g', rel));
	RETURN QUERY SELECT 'numeric', same(format('SELECT g, sum(n), avg(n), min(n) FROM %s WHERE n > 20 GROUP BY g', rel));
	IF with_json THEN
		RETURN QUERY SELECT 'json path', same(format('SELECT attributes->>''channel'', count(*), sum(v) FROM %s GROUP BY 1', rel));
		RETURN QUERY SELECT 'json path filter', same(format('SELECT g, count(*) FROM %s WHERE attributes->>''channel'' = ''web'' GROUP BY g', rel));
		RETURN QUERY SELECT 'json map', same(format('SELECT g, sum((metrics->>''m_total'')::float8), count(metrics->>''quality'') FROM %s GROUP BY g', rel));
		RETURN QUERY SELECT 'json map key', same(format('SELECT g, max((metrics->''quality'')::float8) FROM %s WHERE (metrics->>''m_total'')::float8 > 100 GROUP BY g', rel));
	END IF;
	RETURN QUERY SELECT 'total', same(format('SELECT count(*), sum(v), max(at) FROM %s', rel));
END $$;

SELECT * FROM checks('ic', true);
SELECT * FROM checks('it', false);
SET sift.test_col_rel_inval = on;
SELECT * FROM checks('ic', true);
SELECT * FROM checks('it', false);
RESET sift.test_col_rel_inval;
DROP TABLE ih, ic, it;
DROP FUNCTION checks(text, boolean), same(text);
DROP EXTENSION pg_sift;
