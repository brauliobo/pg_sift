-- mode() WITHIN GROUP counts the (group, value) pairs instead of sorting every value (agg/gen_mode.c): the rows agree with the normal plan on ties
-- (the first value in the order of the call wins, so DESC takes the last), NULLs, groups with no value, a FILTER, collations, a NaN, one group,
-- and when the counts outgrow work_mem and the values go to the sort (case 16: the early rows hold the winning counts, the late rows only add distinct values).
CREATE EXTENSION pg_sift;

SET max_parallel_workers_per_gather = 0;
SET work_mem = '64MB';
SET sift.agg_force = on;

CREATE TABLE m (id int, g int, t text, tc text COLLATE "C", n int, b bigint, d date, f float8, ok bool) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO m SELECT i, i % 9,
	CASE WHEN i % 11 = 0 THEN NULL ELSE 'v' || (i * 7 % 13) END,
	CASE WHEN i % 7 = 0 THEN NULL ELSE chr(65 + (i * 5 % 4)) || chr(97 + i % 3) END,
	CASE WHEN i % 13 = 0 THEN NULL WHEN i % 9 = 8 THEN NULL ELSE i * 3 % 17 - 8 END,
	(i * 31 % 7)::bigint * 4000000000,
	DATE '2020-01-01' + (i * 3 % 5),
	CASE WHEN i % 50 = 0 THEN 'NaN'::float8 WHEN i % 6 = 0 THEN NULL ELSE (i % 4) * 0.5 END,
	i % 3 = 0
FROM generate_series(1, 20000) i;
CREATE INDEX m_col ON m USING sift_col (id, g, t, tc, n, b, d, f, ok);
VACUUM ANALYZE m;

CREATE FUNCTION mo_rows(q text, push bool) RETURNS jsonb[] LANGUAGE plpgsql AS $$
DECLARE r jsonb[];
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT array_agg(to_jsonb(q) ORDER BY q.k) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION mo_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a jsonb[]; b jsonb[];
BEGIN
	a := mo_rows(q, true);
	b := mo_rows(q, false);
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same ' || coalesce(cardinality(a), 0) ELSE 'differs: ' || a::text || ' vs ' || b::text END;
END $$;

CREATE FUNCTION mo_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; r bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r OR l LIKE '%Custom Scan (SiftAgg)%';
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE mo_q (n int, q text);
INSERT INTO mo_q VALUES
	(1, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY t) AS a FROM m GROUP BY g'),
	(2, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY t DESC) AS a FROM m GROUP BY g'),
	(3, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY tc) AS a, mode() WITHIN GROUP (ORDER BY tc COLLATE "C" DESC) AS b FROM m GROUP BY g'),
	(4, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY n) AS a, mode() WITHIN GROUP (ORDER BY n DESC) AS b FROM m GROUP BY g'),
	(5, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY b) AS a, mode() WITHIN GROUP (ORDER BY d) AS c FROM m GROUP BY g'),
	(6, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY f) AS a, mode() WITHIN GROUP (ORDER BY f DESC) AS b FROM m GROUP BY g'),
	(7, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY ok) AS a FROM m GROUP BY g'),
	(8, 'SELECT 1 AS k, mode() WITHIN GROUP (ORDER BY t) AS a, mode() WITHIN GROUP (ORDER BY n) AS b FROM m'),
	(9, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY n) FILTER (WHERE id % 4 = 0) AS a, mode() WITHIN GROUP (ORDER BY t) FILTER (WHERE id < 30) AS b FROM m GROUP BY g'),
	(10, 'SELECT id AS k, mode() WITHIN GROUP (ORDER BY n) AS a, mode() WITHIN GROUP (ORDER BY t) AS b FROM m WHERE id <= 2000 GROUP BY id'),
	(11, 'SELECT id % 2000 AS k, mode() WITHIN GROUP (ORDER BY t) AS a, mode() WITHIN GROUP (ORDER BY n + 1) AS b FROM m GROUP BY id % 2000'),
	(12, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY t) AS a FROM m WHERE id < 0 GROUP BY g'),
	(13, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY n) AS a, count(*) AS c, sum(n) AS s FROM m WHERE n IS NULL OR id < 40 GROUP BY g'),
	(14, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY lower(t)) AS a FROM m GROUP BY g'),
	(15, 'SELECT id % 3000 AS k, mode() WITHIN GROUP (ORDER BY least(id / 3000, 1)) AS a, mode() WITHIN GROUP (ORDER BY least(id / 3000, 1) DESC) AS b FROM m GROUP BY id % 3000'),
	(16, 'SELECT id % 20 AS k, mode() WITHIN GROUP (ORDER BY CASE WHEN id <= 14000 THEN ''zzz'' || id % 20 ELSE repeat(''y'', 400) || id END) AS a FROM m GROUP BY id % 20');

SELECT n, mo_scan(q) AS scan, mo_same(q) AS same FROM mo_q ORDER BY n;

-- the counts outgrow work_mem: the values move to the sort and the rows are the same
SET work_mem = '64kB';
SELECT n, mo_same(q) AS same FROM mo_q WHERE n IN (1, 4, 10, 11, 14, 15, 16) ORDER BY n;
SET sift.enable_agg = on;
SET plan_cache_mode = force_generic_plan;
PREPARE mo_spill AS SELECT count(a) FROM (SELECT id % 6000 AS k, mode() WITHIN GROUP (ORDER BY t) AS a FROM m GROUP BY id % 6000) q;
EXECUTE mo_spill;
SET client_min_messages = debug1;
EXECUTE mo_spill;
RESET client_min_messages;

DROP TABLE mo_q;
DROP FUNCTION mo_rows(text, bool), mo_same(text), mo_scan(text);
DROP TABLE m;
DROP EXTENSION pg_sift;
