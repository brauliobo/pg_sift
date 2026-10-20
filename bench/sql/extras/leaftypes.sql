-- The wide leaf types of 3.2 against the narrow path: index size, CREATE INDEX time and two filters on every type.
--   psql -v n=1000000 -v wide=1 -f leaftypes.sql      (wide=0: the narrow side only, for a build that has no wide leaf types)
-- Every column holds the same values v (an integer in [0, 1e8)) in another type: n18 numeric(18,2) = v / 100, n30 numeric(30,6) and nu numeric (the same
-- value, two lanes and a canonical image), m8 bigint = v cents and mny money = v / 100, iv8 bigint = v microseconds and ivl interval = v microseconds, so a
-- predicate on one of a pair selects the same rows as on the other. One index per side (narrow: id, grp, n18, m8, iv8; wide: id, grp, n30, nu, mny, ivl) is
-- built and measured alone. Output: lines `ROW case arm metric value note`, tab separated.
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
\pset fieldsep '\t'
\if :{?wide} \else \set wide 1 \endif
\ir lib.sql
CREATE OR REPLACE FUNCTION plan_node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	RETURN CASE WHEN p LIKE '%SiftAgg%' THEN 'SiftAgg' WHEN p LIKE '%Index%' THEN 'Index Scan' ELSE 'Seq Scan' END;
END $$;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

DROP TABLE IF EXISTS lt;
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
CREATE TABLE lt AS
SELECT id, grp, v,
       (v / 100.0)::numeric(18,2) AS n18, v AS m8, v AS iv8
FROM (SELECT g AS id, 1 + floor(pg_temp.u(g, 1) * 200)::int AS grp, floor(pg_temp.u(g, 2) * 1e8)::bigint AS v FROM generate_series(1::bigint, :n) g) t;
\if :wide
-- a copy, not an UPDATE: the old versions would keep the heap pages from being all-visible while a transaction of the host is older than the update,
-- and the scan would check the rows in the heap (Rows Vetted By Heap), which is not what the figures compare
CREATE TABLE lt_wide AS
SELECT lt.*, (v / 100.0)::numeric(30,6) AS n30, (v / 100.0)::numeric AS nu, (v / 100.0)::numeric::money AS mny, (v || ' microseconds')::interval AS ivl FROM lt;
DROP TABLE lt;
ALTER TABLE lt_wide RENAME TO lt;
\endif
VACUUM (FREEZE, ANALYZE) lt;
SELECT percentile_disc(.9) WITHIN GROUP (ORDER BY v) AS p90, percentile_disc(.4) WITHIN GROUP (ORDER BY v) AS p40, percentile_disc(.41) WITHIN GROUP (ORDER BY v) AS p41 FROM lt \gset

-- the literal of v in the type of the column
CREATE FUNCTION pg_temp.pred(col text, op text, v bigint) RETURNS text LANGUAGE sql AS $$
	SELECT col || ' ' || op || ' ' || CASE col WHEN 'n18' THEN (v / 100.0)::numeric(18,2)::text WHEN 'n30' THEN (v / 100.0)::numeric(30,6)::text WHEN 'nu' THEN (v / 100.0)::numeric::text
		WHEN 'mny' THEN quote_literal((v / 100.0)::numeric::text) || '::money' WHEN 'ivl' THEN quote_literal(v || ' microseconds') || '::interval' ELSE v::text END $$;
CREATE TEMP TABLE cases AS
SELECT side, kind, col, q, CASE q WHEN 'gt' THEN pg_temp.pred(col, '>', :p90) ELSE pg_temp.pred(col, '>=', :p40) || ' AND ' || pg_temp.pred(col, '<=', :p41) END AS pred
FROM (VALUES ('narrow', 'numeric(18,2)', 'n18'), ('narrow', 'int8 (cents)', 'm8'), ('narrow', 'int8 (microseconds)', 'iv8'),
             ('wide', 'numeric(30,6)', 'n30'), ('wide', 'numeric', 'nu'), ('wide', 'money', 'mny'), ('wide', 'interval', 'ivl')) c(side, kind, col)
CROSS JOIN (VALUES ('gt'), ('btw')) qs(q);

SELECT * FROM build_index('CREATE INDEX lt_narrow ON lt USING sift_col (id, grp, n18, m8, iv8)') \gset b_
SELECT 'ROW', 'index:narrow', 'build', 'wall_ms', :b_wall_ms, '';
SELECT 'ROW', 'index:narrow', 'build', 'cpu_ms', :b_cpu_ms, '';
SELECT 'ROW', 'index:narrow', 'sift_col', 'bytes', pg_relation_size('lt_narrow'), '';
SELECT 'ROW', 'narrow:' || kind, q || ' ' || arm, 'ms', timed_ms(format('SELECT count(*), sum(grp) FROM lt WHERE %s', pred), 21, settings), CASE WHEN arm = 'siftagg' THEN plan_node(format('SELECT count(*), sum(grp) FROM lt WHERE %s', pred)) ELSE '' END
FROM cases CROSS JOIN LATERAL (VALUES ('normal', 'sift.enable_agg=off'), ('siftagg', 'sift.enable_agg=on')) a(arm, settings) WHERE side = 'narrow' ORDER BY kind, q, arm;
DROP INDEX lt_narrow;

\if :wide
SELECT * FROM build_index('CREATE INDEX lt_wide ON lt USING sift_col (id, grp, n30, nu, mny, ivl)') \gset w_
SELECT 'ROW', 'index:wide', 'build', 'wall_ms', :w_wall_ms, '';
SELECT 'ROW', 'index:wide', 'build', 'cpu_ms', :w_cpu_ms, '';
SELECT 'ROW', 'index:wide', 'sift_col', 'bytes', pg_relation_size('lt_wide'), '';
SELECT 'ROW', 'wide:' || kind, q || ' ' || arm, 'ms', timed_ms(format('SELECT count(*), sum(grp) FROM lt WHERE %s', pred), 21, settings), CASE WHEN arm = 'siftagg' THEN plan_node(format('SELECT count(*), sum(grp) FROM lt WHERE %s', pred)) ELSE '' END
FROM cases CROSS JOIN LATERAL (VALUES ('normal', 'sift.enable_agg=off'), ('siftagg', 'sift.enable_agg=on')) a(arm, settings) WHERE side = 'wide' ORDER BY kind, q, arm;
\endif
