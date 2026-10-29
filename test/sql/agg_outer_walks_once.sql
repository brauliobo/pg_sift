-- The scan asks of each sub-select it would join, and of the owner of each CTE it meets, whether it reads a column, an aggregate or a CTE of a query above (a plan for
-- each row, not a list). That is a walk of the whole tree of the sub-select, and the inline plan of a statement repeats a CTE body at each of its references:
-- a statement whose WITH is in a derived table, read by several aggregates that are joined, walked the same trees once for each join of each arm (950 times
-- 9 million instructions in a statement of 17 aggregates, 4.6 s of planning for 5 ms of execution). One planner run makes each walk once.
-- The function sift._test_agg_outer_walks counts the questions and the walks made to answer them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_verdict_cache = off;

CREATE TABLE ow_fact (id int PRIMARY KEY, k int NOT NULL, grp int NOT NULL, v int NOT NULL, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ow_fact SELECT i, i % 3000, i % 40, i % 17, jsonb_build_object('a', i % 9, 'b', i % 5) FROM generate_series(1, 60000) i;
CREATE INDEX ow_fact_col ON ow_fact USING sift_col (id, k, grp, v, doc) WITH (paths = 'doc.* float8');
CREATE TABLE ow_keys (k int PRIMARY KEY);
INSERT INTO ow_keys SELECT g FROM generate_series(1, 30) g;
VACUUM (FREEZE, ANALYZE) ow_fact, ow_keys;
RESET client_min_messages;
\ir test/agg_helpers.sql

-- six aggregates over one CTE of the rows of a few keys, joined one to the next, in a derived table (the WITH is not the statement's own)
CREATE FUNCTION ow_stmt(arms int, wrapped bool) RETURNS text LANGUAGE sql AS $f$ SELECT format($q$
 %s WITH ks AS (SELECT k FROM ow_keys WHERE k <= 20),
  lk AS (SELECT k, count(*) AS n FROM ow_keys GROUP BY k),
  r AS (SELECT f.* FROM ow_fact f JOIN ks USING (k)),
  %s
 SELECT a0.grp, %s
 FROM a0 %s %s $q$,
  CASE WHEN wrapped THEN 'SELECT count(*) AS n, sum(coalesce(c0, 0)) AS total FROM (' ELSE '' END,
  (SELECT string_agg(format('a%s AS (SELECT grp, count(*) AS c%s, sum(x.value::float8) AS s%s FROM r JOIN lk ON lk.k = r.k CROSS JOIN LATERAL jsonb_each_text(r.doc) x WHERE v %% %s = 0 AND r.k IN (SELECT k FROM lk WHERE n > 0) GROUP BY grp)', i, i, i, i + 2), ', ') FROM generate_series(0, arms - 1) i),
  (SELECT string_agg(format('a%s.c%s', i, i), ', ') FROM generate_series(0, arms - 1) i),
  (SELECT string_agg(format('JOIN a%s USING (grp)', i), ' ') FROM generate_series(1, arms - 1) i),
  CASE WHEN wrapped THEN ') t' ELSE '' END) $f$;

CREATE FUNCTION ow_walks(q text) RETURNS bigint[] LANGUAGE plpgsql AS $f$
DECLARE l text; n bigint;
BEGIN
	PERFORM sift._test_agg_outer_walks(true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP NULL; END LOOP;
	RETURN sift._test_agg_outer_walks(true);
END $f$;

-- the planner asks the question for the same sub-select again and again (once for each join of each arm of each plan) and walks it once: the wrapped statement of
-- six arms asks 12 times and walks once (before: 12 walks); the plain statement has no sub-select to ask of
SELECT w[2] < w[1] AS wrapped_asks_answered_once, w[2] <= 3 AS wrapped_walks_bounded FROM ow_walks(ow_stmt(6, true)) w;
SELECT w[2] <= w[1] AS plain_walks_not_more_than_asks, w[2] <= 3 AS plain_walks_bounded FROM ow_walks(ow_stmt(6, false)) w;

-- the scan serves both statements when the table scans are off
SELECT runs_in_scan(ow_stmt(4, false)) AS plain_scanned, runs_in_scan(ow_stmt(4, true)) AS wrapped_scanned;

-- the rows are the same with the scan on and off, for no key, one, a few and all of them, with NULL and empty documents and updates and deletes between the runs
CREATE FUNCTION ow_same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $f$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO result_rows FROM r_on;
	SELECT count(*) INTO vs_normal_plan FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $f$;
-- the plain form returns a row for each group, the derived table one row of totals
CREATE FUNCTION ow_check(step text) RETURNS TABLE (label text, result_rows bigint, differing bigint) LANGUAGE plpgsql AS $f$
DECLARE kmax int; w bool;
BEGIN
	FOREACH kmax IN ARRAY ARRAY[0, 1, 5, 20, 3000] LOOP
		FOREACH w IN ARRAY ARRAY[false, true] LOOP
			label := format('%s keys<=%s %s', step, kmax, CASE WHEN w THEN 'wrapped' ELSE 'plain' END);
			SELECT s.result_rows, s.vs_normal_plan INTO result_rows, differing
				FROM ow_same(replace(ow_stmt(4, w), 'k <= 20', 'k <= ' || kmax)) s;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $f$;
SELECT * FROM ow_check('initial');
UPDATE ow_fact SET doc = NULL WHERE id % 11 = 0;
UPDATE ow_fact SET doc = '{}' WHERE id % 13 = 0;
UPDATE ow_fact SET doc = doc || '{"a": null}' WHERE id % 7 = 0 AND doc IS NOT NULL;
UPDATE ow_fact SET grp = (grp + 1) % 40 WHERE id % 17 = 0;
DELETE FROM ow_fact WHERE id % 19 = 0;
INSERT INTO ow_fact SELECT 100000 + i, i % 3000, i % 40, i % 17, jsonb_build_object('a', i % 4) FROM generate_series(1, 3000) i;
SELECT * FROM ow_check('after dml');
VACUUM (ANALYZE) ow_fact;
SELECT * FROM ow_check('after vacuum');

DROP FUNCTION ow_check(text), ow_same(text), ow_walks(text), ow_stmt(int, bool), outcome(text, bool), runs_in_scan(text), pushed, runq;
DROP TABLE ow_fact, ow_keys;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
