-- Format 1 on the unified aggregate scan (sift.test_unified_agg): a column no native vector holds (char(n), an enum, an array, a numeric of any form, money, an interval, a uuid) is folded as the
-- datums it is, in a key, an aggregate argument, the condition of a FILTER and the key of the Top-N of the rows of a partitioned table, and its groups are the heap's own.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = on;
SET sift.test_unified_agg = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE d (id int PRIMARY KEY, grp int, ch char(3), mo mood, ia int[], n numeric(12, 2), nf numeric, m money, iv interval, u uuid);
INSERT INTO d
SELECT i, i % 5, (ARRAY['a', 'bb', 'ccc'])[i % 3 + 1], (ARRAY['sad', 'ok', 'happy'])[i % 3 + 1]::mood, ARRAY[i % 4, i % 2],
	CASE WHEN i % 97 = 0 THEN NULL ELSE (i % 250) / 4.0 END, CASE WHEN i % 89 = 0 THEN 'NaN'::numeric ELSE i / 8.0 END,
	(i % 40)::numeric::money, (i % 6 || ' days')::interval, ('00000000-0000-0000-0000-' || lpad((i % 7)::text, 12, '0'))::uuid
FROM generate_series(1, 2000) i;
CREATE INDEX d_col ON d USING sift_col (id, grp, ch, mo, ia, n, nf, m, iv, u);
VACUUM (FREEZE, ANALYZE) d;

CREATE TABLE p (id int, placed date, total numeric(12, 2)) PARTITION BY RANGE (placed);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM ('2024-01-01') TO ('2024-07-01');
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM ('2024-07-01') TO ('2025-01-01');
INSERT INTO p SELECT i, date '2024-01-01' + i % 360, (i * 37 % 1000) / 4.0 FROM generate_series(1, 2000) i;
CREATE INDEX p1_col ON p1 USING sift_col (id, placed, total);
CREATE INDEX p2_col ON p2 USING sift_col (id, placed, total);
VACUUM (FREEZE, ANALYZE) p;

-- the node the plan of q has, and whether its rows are those of the heap (sift.enable_agg = off)
CREATE FUNCTION check_q(q text) RETURNS TABLE (node text, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; p text := ''; a text; b text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO b;
	node := coalesce((regexp_match(p, 'Custom Scan \((Sift\w+)\)'))[1], 'none');
	same := a = b;
	RETURN NEXT;
END $$;

SELECT * FROM check_q($$SELECT ch, count(*) FROM d GROUP BY ch$$);
SELECT * FROM check_q($$SELECT mo, count(*), max(mo) FROM d GROUP BY mo$$);
SELECT * FROM check_q($$SELECT ia, count(*) FROM d GROUP BY ia$$);
SELECT * FROM check_q($$SELECT m, count(*), sum(m), max(m) FROM d GROUP BY m$$);
SELECT * FROM check_q($$SELECT iv, count(*), max(iv) FROM d GROUP BY iv$$);
SELECT * FROM check_q($$SELECT u, count(*) FROM d GROUP BY u$$);
SELECT * FROM check_q($$SELECT grp, bool_or(n > 40), sum(n) FILTER (WHERE n > 40), avg(n) FILTER (WHERE n < 10) FROM d GROUP BY grp$$);
SELECT * FROM check_q($$SELECT grp, count(*) FILTER (WHERE nf > 5 OR iv > interval '1 day'), sum(nf) FILTER (WHERE m > '10.00') FROM d GROUP BY grp$$);
SELECT * FROM check_q($$SELECT grp, count(DISTINCT n) FILTER (WHERE grp > 1), min(nf), max(nf) FROM d GROUP BY grp$$);
SELECT * FROM check_q($$SELECT id, total FROM p ORDER BY total DESC, id LIMIT 5$$);

DROP FUNCTION check_q(text);
DROP TABLE p;
DROP TABLE d;
DROP TYPE mood;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
