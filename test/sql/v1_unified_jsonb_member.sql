-- Format 1 on the unified aggregate scan (sift.test_unified_agg) over a member of a map of a jsonb document, (doc -> 'metrics' ->> 'm_a')::float8 of the map `doc.metrics.*`: the value is read from the
-- leaves of the index, and from the heap row only for the rows the leaves cannot hold (a value that is no number or out of range, a map the shredder cut, a member whose key it could not intern).
-- Every statement is run with the node and without it (sift.enable_agg) and the answers, or the errors, are compared.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = on;
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE m (id int PRIMARY KEY, grp int, ts date, doc jsonb);
INSERT INTO m
SELECT i, i % 5, date '2024-01-01' + i % 60,
	jsonb_build_object('metrics', jsonb_build_object('m_a', (i % 40) / 4.0 - 3, 'm_b', i % 9), 'other', i)
FROM generate_series(1, 3000) i;
INSERT INTO m VALUES
	(5001, 1, '2024-02-01', NULL),
	(5002, 1, '2024-02-01', '{"other": 1}'),
	(5003, 1, '2024-02-01', '{"metrics": null}'),
	(5004, 1, '2024-02-01', '{"metrics": 3}'),
	(5005, 1, '2024-02-01', '{"metrics": [1, 2]}'),
	(5006, 1, '2024-02-01', '{"metrics": "text"}'),
	(5007, 1, '2024-02-01', '{"metrics": {}}'),
	(5008, 1, '2024-02-01', '{"metrics": {"m_b": 1}}'),
	(5009, 1, '2024-02-01', '{"metrics": {"m_a": null}}'),
	(5010, 1, '2024-02-01', '{"metrics": {"m_a": -0.0}}'),
	(5011, 1, '2024-02-01', '{"metrics": {"m_a": 1e300}}'),
	(5012, 1, '2024-02-01', '{"metrics": {"m_a": 0.1000000000000000055511151231257827}}'),
	(5013, 1, '2024-02-01', '{"metrics": {"m_a": 12345678901234567890123}}'),
	(5014, 1, '2024-02-01', '{"metrics": {"m_a": "2.5"}}'),
	(5015, 1, '2024-02-01', '{"metrics": {"m_a": "abc"}}'),
	(5016, 1, '2024-02-01', '{"metrics": {"m_a": true}}'),
	(5017, 1, '2024-02-01', '{"metrics": {"m_a": {"x": 1}}}'),
	(5018, 1, '2024-02-01', '{"metrics": {"m_a": [1]}}'),
	(5019, 1, '2024-02-01', '{"metrics": {"m_a": 1e400}}'),
	(5020, 1, '2024-02-01', '{"metrics": {"m_a": 1e-400}}'),
	(5021, 1, '2024-02-01', jsonb_build_object('metrics', jsonb_build_object('m_a', 'NaN'::numeric))),
	(5022, 1, '2024-02-01', '{"metrics": {"m_a": 5, "m_a2": "x", "m_a": 7}}'),
	(5023, 1, '2024-02-01', (SELECT jsonb_build_object('metrics', jsonb_object_agg('k' || g, g) || '{"m_a": 9}') FROM generate_series(1, 3000) g));
CREATE INDEX m_col ON m USING sift_col (id, grp, ts, doc) WITH (paths = 'doc.metrics.* float8');
VACUUM (FREEZE, ANALYZE) m;

-- the node the plan of q has, and whether its rows are those of the heap (sift.enable_agg = off)
CREATE FUNCTION check_q(q text) RETURNS TABLE (node text, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; p text := ''; a text; b text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO b;
	SET LOCAL sift.enable_agg = on;
	node := coalesce((regexp_match(p, 'Custom Scan \((Sift\w+)\)'))[1], 'none');
	same := a = b;
	RETURN NEXT;
END $$;

-- the error of q, with the node and without it: the same, or none
CREATE FUNCTION check_err(q text) RETURNS TABLE (node text, same bool, err text) LANGUAGE plpgsql AS $$
DECLARE l text; p text := ''; a text := 'none'; b text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	BEGIN EXECUTE q; EXCEPTION WHEN OTHERS THEN a := SQLERRM; END;
	SET LOCAL sift.enable_agg = off;
	BEGIN EXECUTE q; EXCEPTION WHEN OTHERS THEN b := SQLERRM; END;
	SET LOCAL sift.enable_agg = on;
	node := coalesce((regexp_match(p, 'Custom Scan \((Sift\w+)\)'))[1], 'none');
	same := a = b;
	err := left(b, 60);
	RETURN NEXT;
END $$;

-- the rows the leaves hold, with the others (NULL, scalar and array documents and maps, missing members, JSON nulls, -0, large numbers, a number the leaf cannot hold) kept out by the quals
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8), avg((doc -> 'metrics' ->> 'm_b')::float8), count(*) FROM m WHERE id <= 3000 GROUP BY 1$$);
SELECT * FROM check_q($$SELECT grp, round(avg((doc -> 'metrics' ->> 'm_a')::float8)::numeric, 3), count(*) FROM m WHERE id <= 3000 AND ts >= date '2024-01-10' GROUP BY 1$$);
SELECT * FROM check_q($$SELECT (doc -> 'metrics' ->> 'm_a')::float8 AS a, count(*) FROM m WHERE id <= 3000 GROUP BY 1$$);
SELECT * FROM check_q($$SELECT count((doc -> 'metrics' ->> 'm_a')::float8), min((doc -> 'metrics' ->> 'm_a')::float8), max((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id <= 3000$$);
SELECT * FROM check_q($$SELECT 'a' AS arm, avg((doc -> 'metrics' ->> 'm_a')::float8), count(*) FROM m WHERE id <= 3000 UNION ALL SELECT 'b', avg((doc -> 'metrics' ->> 'm_b')::float8), count(*) FROM m WHERE id <= 3000$$);
-- each row of the other kinds on its own: it comes from the heap row, and the answer is PostgreSQL's
SELECT id, r.* FROM generate_series(5001, 5014) id, LATERAL check_q(format($$SELECT sum((doc -> 'metrics' ->> 'm_a')::float8), count((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id = %s$$, id)) r;
SELECT id, r.* FROM generate_series(5015, 5023) id, LATERAL check_err(format($$SELECT sum((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id = %s$$, id)) r;
SELECT id, r.* FROM (VALUES (5021), (5022), (5023)) v(id), LATERAL check_q(format($$SELECT sum((doc -> 'metrics' ->> 'm_a')::float8), count(*) FROM m WHERE id = %s$$, id)) r;
-- all of them together: the first one that raises, as PostgreSQL's
SELECT * FROM check_err($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8) FROM m GROUP BY 1$$);
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id < 5015 OR id = 5021 GROUP BY 1$$);
-- a FILTER and a HAVING over the member
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8) FILTER (WHERE ts >= date '2024-01-20') FROM m WHERE id <= 3000 GROUP BY 1$$);
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id <= 3000 GROUP BY 1 HAVING sum((doc -> 'metrics' ->> 'm_a')::float8) > 0$$);
-- a member the index does not declare, and a member of the document itself, are not read from a map
SELECT * FROM check_q($$SELECT sum((doc -> 'other' ->> 'm_a')::float8) FROM m WHERE id <= 3000$$);
SELECT * FROM check_q($$SELECT sum((doc ->> 'other')::float8) FROM m WHERE id <= 3000$$);

-- rows in the write buffer, updated and deleted rows
INSERT INTO m SELECT 6000 + i, i % 5, date '2024-03-01', jsonb_build_object('metrics', jsonb_build_object('m_a', i / 8.0, 'm_b', i)) FROM generate_series(1, 200) i;
INSERT INTO m VALUES (6300, 2, '2024-03-01', '{"metrics": {"m_a": "bad"}}'), (6301, 2, '2024-03-01', '{"metrics": {"m_a": "4.5"}}');
UPDATE m SET doc = jsonb_set(doc, '{metrics,m_a}', '100') WHERE id BETWEEN 10 AND 20;
UPDATE m SET doc = '{"metrics": {"m_a": "bad"}}' WHERE id = 30;
DELETE FROM m WHERE id BETWEEN 40 AND 60 OR id = 6300;
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8), count(*) FROM m WHERE id <= 3000 AND id <> 30 GROUP BY 1$$);
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8), count(*) FROM m WHERE id >= 6000 GROUP BY 1$$);
SELECT * FROM check_err($$SELECT sum((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id <= 3000$$);
SELECT * FROM check_q($$SELECT sum((doc -> 'metrics' ->> 'm_a')::float8) FROM m WHERE id BETWEEN 40 AND 60$$);
VACUUM m;
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8), count(*) FROM m WHERE id <= 3000 AND id <> 30 GROUP BY 1$$);
SELECT * FROM check_q($$SELECT grp, sum((doc -> 'metrics' ->> 'm_a')::float8), count(*) FROM m WHERE id >= 6000 AND id <> 6300 GROUP BY 1$$);

DROP TABLE m;
DROP FUNCTION check_q, check_err;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
