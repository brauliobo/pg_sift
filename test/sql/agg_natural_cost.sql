-- The plan the planner picks on its own costs (no page cost or enable_* setting steers it): the aggregate scan on a table of many rows when
-- the scope keeps a large share of it, the btree plan when the scope is a few entities. The statements are the shapes of review analytics: a
-- date spine (a set-returning function in the select list) joined to the rows of two periods with no key the scan can use, and the same
-- aggregate without it. Every statement is also run with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE nc_entities (id int PRIMARY KEY, kind text NOT NULL);
INSERT INTO nc_entities SELECT i, (ARRAY['hotel', 'museum', 'diner'])[1 + i % 3] FROM generate_series(1, 400) i;
-- written entity by entity, as a crawler appends them; the day is not ordered
CREATE TABLE nc_rows (id int PRIMARY KEY, entity_id int NOT NULL, day date NOT NULL, deleted_at timestamptz, scores jsonb) WITH (autovacuum_enabled = false);
INSERT INTO nc_rows SELECT i, 1 + (i - 1) / 500, date '2025-01-01' + (hashint4(i) & 1023) % 700, CASE WHEN i % 29 = 0 THEN now() END,
	jsonb_build_object('service', 1 + i % 10, 'value', 1 + (i * 7) % 10, 'location', 1 + (i * 3) % 10) FROM generate_series(1, 200000) i;
CREATE INDEX nc_rows_entity ON nc_rows (entity_id, day);
CREATE INDEX nc_rows_col ON nc_rows USING sift_col (id, entity_id, day, deleted_at, scores) WITH (paths = 'scores.* float8');
VACUUM (FREEZE, ANALYZE) nc_rows;
ANALYZE nc_entities;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN n := n + 1; END IF;
	END LOOP;
	RETURN CASE n WHEN 0 THEN 'normal' WHEN 1 THEN 'SiftAgg' ELSE n || ' SiftAgg' END;
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

CREATE FUNCTION spine(scope text) RETURNS text LANGUAGE sql AS $$ SELECT format($f$
WITH ids AS MATERIALIZED (SELECT id FROM nc_entities WHERE id %s),
periods AS (SELECT * FROM (VALUES ('current', daterange('2026-03-01', '2026-06-30', '[]'), interval '0 day'),
	('previous', daterange('2025-03-01', '2025-06-30', '[]'), interval '1 year')) p(period, range, shift)),
spine AS (SELECT period, (generate_series(date_trunc('month', lower(range))::date, date_trunc('month', upper(range) - 1)::date, interval '1 month') + shift)::date AS bucket FROM periods),
scoped AS (SELECT r.*, p.period AS bucket_period, (date_trunc('month', r.day) + p.shift)::date AS bucket_date
	FROM nc_rows r JOIN periods p ON r.day <@ p.range JOIN ids ON ids.id = r.entity_id
	WHERE r.deleted_at IS NULL AND ((r.day >= '2026-03-01' AND r.day <= '2026-06-30') OR (r.day >= '2025-03-01' AND r.day <= '2025-06-30'))),
parsed AS (SELECT s.id, s.bucket_period, s.bucket_date, e.value::float8 AS score FROM scoped s JOIN LATERAL jsonb_each(s.scores) e ON true)
SELECT d.period, d.bucket, count(DISTINCT x.id) AS ids, round(avg(x.score)::numeric, 6) AS score
FROM spine d LEFT JOIN parsed x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.bucket)
GROUP BY d.period, d.bucket ORDER BY 1, 2$f$, scope) $$;

-- a large scope: most of the table is read, the scan is cheaper than the btree plan and its date spine does not make it dearer
SELECT * FROM same(spine('<= 300'));
-- a few entities: the btree plan reads a few hundred rows
SELECT * FROM same(spine('= 7'));

CREATE FUNCTION grouped(scope text) RETURNS text LANGUAGE sql AS $$ SELECT format($f$
SELECT e.key, count(DISTINCT r.id) AS ids, round(avg(e.value::float8)::numeric, 6) AS score FROM nc_rows r JOIN LATERAL jsonb_each(r.scores) e ON true
WHERE r.deleted_at IS NULL AND r.entity_id %s GROUP BY e.key ORDER BY 1
$f$, scope) $$;

-- the same without the spine; a month of one entity is a few rows for the btree plan
SELECT * FROM same(grouped('<= 300'));
SELECT * FROM same(grouped('= 7 AND r.day BETWEEN date ''2025-03-01'' AND date ''2025-03-31'''));
DROP FUNCTION grouped(text), spine(text), same(text), plan_of(text);
DROP TABLE nc_rows, nc_entities;
DROP EXTENSION pg_sift;
