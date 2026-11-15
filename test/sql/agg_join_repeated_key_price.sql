-- A table whose tests have few outcomes is kept as classes of its keys only if no key repeats (a class is the key's): a table with several rows for a key is read as rows into a
-- hash table of its keys, which costs a row each, past the cache a miss each. The planner prices the classes only for a table that can be one, and the directory of the keys of a
-- table of rows by its distinct keys. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE rk_once (k int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO rk_once SELECT g, g % 7 FROM generate_series(1, 20000) g;
CREATE TABLE rk_many (id int, k int, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO rk_many SELECT g, 1 + g % 20000, (1 + g % 20000) % 7 FROM generate_series(1, 200000) g;
CREATE TABLE rk_fact (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO rk_fact SELECT g, 1 + (g * 7) % 20000, g % 100 FROM generate_series(1, 400000) g;
SET client_min_messages = warning;
CREATE INDEX rk_once_col ON rk_once USING sift_col (k, a);
CREATE INDEX rk_many_col ON rk_many USING sift_col (id, k, a);
CREATE INDEX rk_fact_col ON rk_fact USING sift_col (id, k, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) rk_once, rk_many, rk_fact;

CREATE FUNCTION scan_cost(t text) RETURNS float8 LANGUAGE sql AS $$
	SELECT scan_cost FROM sift.explain_why(format($q$SELECT sum(f.v) FILTER (WHERE d.a = 1), sum(f.v) FILTER (WHERE d.a < 3) FROM rk_fact f JOIN %s d ON d.k = f.k$q$, t))
$$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT scan_cost('rk_many') > 1.5 * scan_cost('rk_once') AS repeated_keys_cost_more;
SELECT answer(format($q$SELECT d.a, count(*), sum(f.v) FROM rk_fact f JOIN %s d ON d.k = f.k GROUP BY 1$q$, t), true) =
       answer(format($q$SELECT d.a, count(*), sum(f.v) FROM rk_fact f JOIN %s d ON d.k = f.k GROUP BY 1$q$, t), false) AS same_answers, t
FROM (VALUES ('rk_once'), ('rk_many')) v (t) ORDER BY t;

DROP FUNCTION scan_cost(text), answer(text, bool);
DROP TABLE rk_fact, rk_many, rk_once;
DROP EXTENSION pg_sift;
