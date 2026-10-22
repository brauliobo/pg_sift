-- SiftAgg over a scope of a few keys that are spread over every unit of the table: the filters of the source leave a handful of rows in each unit, and
-- the scan goes on with those rows alone, renumbered as for a match set (the leaves, the elements of a list, the dead and updated rows, the
-- distinct counts and the rows PostgreSQL reads again from the heap). Every statement is also run with sift.enable_agg = off: the rows must be the same,
-- and the plan of each is SiftAgg.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE TABLE nr_facts (id int, parent_id int, ts date, kind text, amount int, score int, parts jsonb) WITH (autovacuum_enabled = false);
INSERT INTO nr_facts
SELECT i, 1 + (i * 7919) % 120, date '2024-01-01' + i / 100, 'cat_' || chr(97 + i % 4), (i * 104729::bigint) % 1000, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 5 END,
	   CASE WHEN i % 13 = 0 THEN NULL
			WHEN i % 17 = 0 THEN '[]'
			ELSE jsonb_build_array(jsonb_build_object('asp', 'asp_' || chr(97 + i % 6), 'sign', 's' || (i % 3), 'score', CASE WHEN i % 1511 = 0 THEN to_jsonb('3'::text) ELSE to_jsonb(i % 5) END),
								   jsonb_build_object('asp', 'asp_' || chr(97 + (i + 2) % 6), 'sign', 's' || (i % 2), 'score', (i + 1) % 5)) ||
				 CASE WHEN i % 7 = 0 THEN jsonb_build_array(jsonb_build_object('asp', 'asp_a', 'sign', NULL, 'score', 1)) ELSE '[]' END END
FROM generate_series(1, 40000) i;
CREATE INDEX nr_facts_col ON nr_facts USING sift_col (id, parent_id, ts, kind, amount, score, parts)
	WITH (paths = 'parts[]{asp text, sign text, score int4}');
CREATE TABLE nr_dims (id int PRIMARY KEY, payload jsonb NOT NULL);
INSERT INTO nr_dims SELECT p, jsonb_build_object('k', 'k' || p, 'tier', 'tier_' || chr(97 + p % 5)) FROM generate_series(1, 120) p;
VACUUM (FREEZE, ANALYZE) nr_facts, nr_dims;
-- rows of the scope that die and change after the vacuum: their units are not all visible, and a dirty element comes from the heap
DELETE FROM nr_facts WHERE parent_id = 7 AND id % 9 = 0;
UPDATE nr_facts SET amount = amount + 1, score = 3 WHERE parent_id IN (7, 8) AND id % 5 = 1;
UPDATE nr_facts SET parts = jsonb_build_array(jsonb_build_object('asp', 'asp_b', 'sign', 's9', 'score', to_jsonb('4'::text))) WHERE parent_id = 7 AND id % 23 = 0 AND id < 9000;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN CASE WHEN j::text LIKE '%"SiftAgg"%' THEN 'SiftAgg' ELSE 'normal' END;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE nr_q (n int, shape text, q text);
INSERT INTO nr_q VALUES
	(1, 'columns',       $q$SELECT f.kind, count(*) AS n, sum(f.amount) AS s, min(f.amount) AS lo, max(f.amount) AS hi, avg(f.score) AS a FROM nr_facts f
						    WHERE f.parent_id IN (SELECT d.id FROM nr_dims d WHERE %s) AND f.ts >= date '2024-02-01' AND f.ts < date '2024-12-01' GROUP BY 1$q$),
	(2, 'no group',      $q$SELECT count(*) AS n, count(f.score) AS c, sum(f.score) AS s FROM nr_facts f WHERE f.parent_id IN (SELECT d.id FROM nr_dims d WHERE %s)$q$),
	(3, 'distinct',      $q$SELECT f.score, count(DISTINCT f.ts) AS days, count(DISTINCT f.kind) AS kinds FROM nr_facts f
						    WHERE f.parent_id IN (SELECT d.id FROM nr_dims d WHERE %s) GROUP BY 1$q$),
	(4, 'record set',    $q$SELECT e.asp, e.sign, count(*) AS n, sum(e.score) AS s FROM nr_facts f CROSS JOIN LATERAL jsonb_to_recordset(f.parts) e(asp text, sign text, score int)
						    WHERE f.parent_id IN (SELECT d.id FROM nr_dims d WHERE %s) GROUP BY 1, 2$q$),
	(5, 'element test',  $q$SELECT f.kind, e.asp, count(*) AS n FROM nr_facts f CROSS JOIN LATERAL jsonb_to_recordset(f.parts) e(asp text, sign text, score int)
						    WHERE f.parent_id IN (SELECT d.id FROM nr_dims d WHERE %s) AND e.score > 1 AND f.ts >= date '2024-03-01' GROUP BY 1, 2$q$),
	(6, 'list distinct', $q$SELECT f.kind, count(DISTINCT e.asp) AS asps, count(DISTINCT f.id) AS ids, count(*) FILTER (WHERE e.sign = 's1') AS s1 FROM nr_facts f
						    CROSS JOIN LATERAL jsonb_to_recordset(f.parts) e(asp text, sign text, score int) WHERE f.parent_id IN (SELECT d.id FROM nr_dims d WHERE %s) GROUP BY 1$q$);

CREATE FUNCTION scope(n int, filter text) RETURNS text LANGUAGE sql AS $$ SELECT format(q, filter) FROM nr_q WHERE nr_q.n = scope.n $$;

-- one key, three keys, a fifth of the parents (the unit is not narrowed: it keeps more than a 32nd of its rows), none
SELECT n, shape, s.* FROM nr_q, LATERAL same(scope(n, $f$d.payload ->> 'k' = 'k7'$f$)) s ORDER BY n;
SELECT n, shape, s.* FROM nr_q, LATERAL same(scope(n, $f$d.payload ->> 'k' IN ('k7', 'k8', 'k9')$f$)) s ORDER BY n;
SELECT n, shape, s.* FROM nr_q, LATERAL same(scope(n, $f$d.payload ->> 'tier' = 'tier_c'$f$)) s ORDER BY n;
SELECT n, shape, s.* FROM nr_q, LATERAL same(scope(n, $f$d.payload ->> 'k' = 'none'$f$)) s ORDER BY n;

DROP FUNCTION scope(int, text), same(text), plan_of(text);
DROP TABLE nr_q, nr_facts, nr_dims;
DROP EXTENSION pg_sift;
