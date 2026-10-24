-- `jsonb_build_array(a1, ..) = jsonb_build_array(b1, ..)` as the ON clause of a lookup join: the arrays are equal when their elements are, a NULL
-- being equal to a NULL, so each element of an integer, date or text type that equals a column of the joined table is a hash key of the lookup
-- (a NULL key finds the rows whose key is NULL); the other elements stay a condition of the table's rows. Every query runs as the scan and as the
-- normal plan, and the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE facts (id int PRIMARY KEY, parent_id int, kind text, ts date, amount numeric, val float8) WITH (autovacuum_enabled = false);
INSERT INTO facts
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 9 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 4 END,
       CASE WHEN i % 13 = 0 THEN NULL ELSE date '2025-01-01' + (i / 3) % 6 END, CASE WHEN i % 3 = 0 THEN round(i % 5 + 0.5, 1) ELSE round(i % 5 + 0.5, 2) END, i % 7
FROM generate_series(1, 6000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, parent_id, kind, ts, amount, val);
VACUUM (FREEZE, ANALYZE) facts;

-- buckets: a NULL kind and a NULL day (they equal the facts without one), a bucket twice, buckets no fact has, one with a numeric amount of another scale
CREATE TABLE buckets (kind text, ts date, pid int, flag bool, amt numeric, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', '2025-01-01', 1, true, 1.5, 'a'), ('k0', '2025-01-02', 2, true, 2.50, 'b'), ('k1', '2025-01-01', 3, false, 3.5, 'c'),
  ('k1', '2025-01-01', 3, false, 3.5, 'd'), ('k2', '2025-01-04', 4, true, 4.5, 'e'), (NULL, '2025-01-03', 5, false, 0.5, 'f'),
  (NULL, NULL, NULL, NULL, NULL, 'g'), ('k9', '2025-01-01', 1, true, 1.50, 'h'), ('k3', NULL, 6, false, 2.5, 'i');
VACUUM (FREEZE, ANALYZE) buckets;

\ir test/agg_helpers.sql

-- the rows of the scan and of the normal plan are the same, and the plan is the scan's
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN d = 0 AND n > 0 AND pushed(q);
END $$;

-- the aggregates over the joined rows; the join kind and the ON clause are the checks'
CREATE FUNCTION report(spine_first bool, jointype text, cond text, keys text DEFAULT 'b.label') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$SELECT %s, count(*) AS n, count(f.id) AS facts, count(DISTINCT f.id) AS ids, sum(f.amount) AS amount, sum(f.val) AS val
	                  FROM %s GROUP BY %s$q$, keys,
	              CASE WHEN spine_first THEN format('buckets b %s facts f ON %s', jointype, cond) ELSE format('facts f %s buckets b ON %s', jointype, cond) END, keys)
$$;

-- each ON clause as the preserved spine (buckets LEFT JOIN facts), as a lookup the facts keep their rows of (facts LEFT JOIN buckets) and as an inner join
CREATE FUNCTION each_join(cond text, keys text DEFAULT 'b.label') RETURNS TABLE (shape text, ok bool) LANGUAGE sql AS $$
	SELECT 'preserved', same(report(true, 'LEFT JOIN', cond, keys))
	UNION ALL SELECT 'lookup', same(report(false, 'LEFT JOIN', cond, keys || ', f.kind'))
	UNION ALL SELECT 'inner', same(report(false, 'JOIN', cond, keys))
$$;

-- text and date elements: a NULL equals a NULL (the NULL bucket and the facts without a kind or a day), a bucket twice, a bucket nothing has
SELECT * FROM each_join('jsonb_build_array(f.kind, f.ts) = jsonb_build_array(b.kind, b.ts)');
-- the sides the other way round
SELECT * FROM each_join('jsonb_build_array(b.kind, b.ts) = jsonb_build_array(f.kind, f.ts)');
-- one element is a key, the other the join's own
SELECT * FROM each_join('jsonb_build_array(f.kind) = jsonb_build_array(b.kind)');
-- integer elements, one of an expression of the facts' columns
SELECT * FROM each_join('jsonb_build_array(f.parent_id + 0, f.ts) = jsonb_build_array(b.pid, b.ts)');
-- a boolean element and a numeric one (1.5 and 1.50 are equal numbers) are conditions of the rows, the date a key
SELECT * FROM each_join('jsonb_build_array(f.parent_id > 3, f.ts) = jsonb_build_array(b.flag, b.ts)');
SELECT * FROM each_join('jsonb_build_array(f.amount, f.ts) = jsonb_build_array(b.amt, b.ts)');
SELECT * FROM each_join('jsonb_build_array(f.amount, f.kind, f.ts) = jsonb_build_array(b.amt, b.kind, b.ts)');
-- an element of another type on each side (an integer and a numeric: equal numbers), a constant element, one that no fact has
SELECT * FROM each_join('jsonb_build_array(f.parent_id, f.ts) = jsonb_build_array(b.amt, b.ts)');
SELECT * FROM each_join('jsonb_build_array(f.kind, f.ts) = jsonb_build_array(''k1''::text, b.ts)');
SELECT * FROM each_join('jsonb_build_array(f.kind, f.ts) = jsonb_build_array(b.kind, DATE ''2025-01-04'')');
-- the array next to another condition of the ON clause
SELECT * FROM each_join('jsonb_build_array(f.kind, f.ts) = jsonb_build_array(b.kind, b.ts) AND f.val > 2');
SELECT * FROM each_join('jsonb_build_array(f.kind, f.ts) = jsonb_build_array(b.kind, b.ts) AND b.label <> ''d''');
-- the same columns as plain equalities are the reference: a NULL equals nothing there
SELECT * FROM each_join('f.kind = b.kind AND f.ts = b.ts');
-- groups by a column of the facts as well
SELECT * FROM each_join('jsonb_build_array(f.kind, f.ts) = jsonb_build_array(b.kind, b.ts)', 'b.label, f.ts');
DROP FUNCTION each_join, outcome, pushed, report, runq, runs_in_scan, same;
DROP TABLE buckets, facts;
DROP EXTENSION pg_sift;
