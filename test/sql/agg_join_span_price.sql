-- A table is kept as an array of classes over its keys only if the keys span no more than 8 times its rows plus 65,536 (and the lookup budget): the load reads the whole index
-- before it finds that a sparser table does not, so the planner asks the same question (one function) and prices a sparser table as rows. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE sp_dense (id int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO sp_dense SELECT g, g % 7 FROM generate_series(1, 20000) g;
-- the same rows with keys 100 apart: a span of 2,000,000, over 8 * 20,000 + 65,536
CREATE TABLE sp_wide (id int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO sp_wide SELECT g * 100, g % 7 FROM generate_series(1, 20000) g;
CREATE TABLE sp_fact_dense (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO sp_fact_dense SELECT g, 1 + (g * 7) % 20000, g % 100 FROM generate_series(1, 400000) g;
CREATE TABLE sp_fact_wide (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO sp_fact_wide SELECT id, k * 100, v FROM sp_fact_dense;
SET client_min_messages = warning;
CREATE INDEX sp_dense_col ON sp_dense USING sift_col (id, a);
CREATE INDEX sp_wide_col ON sp_wide USING sift_col (id, a);
CREATE INDEX sp_fact_dense_col ON sp_fact_dense USING sift_col (id, k, v);
CREATE INDEX sp_fact_wide_col ON sp_fact_wide USING sift_col (id, k, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) sp_dense, sp_wide, sp_fact_dense, sp_fact_wide;

CREATE FUNCTION scan_cost(kind text) RETURNS float8 LANGUAGE sql AS $$
	SELECT scan_cost FROM sift.explain_why(format($q$SELECT sum(f.v) FILTER (WHERE p.a = 1), sum(f.v) FILTER (WHERE p.a < 3) FROM sp_fact_%s f JOIN sp_%s p ON p.id = f.k$q$, kind, kind))
$$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT scan_cost('wide') > 1.3 * scan_cost('dense') AS wide_span_costs_more;
SELECT k, answer(format($q$SELECT p.a, sum(f.v) FROM sp_fact_%s f JOIN sp_%s p ON p.id = f.k GROUP BY 1$q$, k, k), true) =
          answer(format($q$SELECT p.a, sum(f.v) FROM sp_fact_%s f JOIN sp_%s p ON p.id = f.k GROUP BY 1$q$, k, k), false) AS same_answers
FROM (VALUES ('dense'), ('wide')) v (k) ORDER BY k;

DROP FUNCTION scan_cost(text), answer(text, bool);
DROP TABLE sp_fact_dense, sp_fact_wide, sp_dense, sp_wide;
DROP EXTENSION pg_sift;
