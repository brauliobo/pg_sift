-- One entity from a CTE of ids, on an index cut by that key (key_cut): the scan reads a handful of units for it. PostgreSQL counts the CTE at one row and the normal plan
-- probes the btree for the key's rows; the planner charged that plan the rows the table's filters keep (a lost estimate, a factor of hundreds of thousands), so the scan
-- won for an entity of a few hundred events, with a lookup table of thousands of rows to load, and lost: 4 times the buffer hits and the time of the probes. A key that is
-- sure to be one is charged only the excess of its rows over the mean's: the small entity keeps the normal plan, the entity that owns a third of the table, which the
-- probes read slowly, keeps the scan. Every statement is also run with sift.enable_agg = off, before and after DML (updates, deletes, a NULL entity, a row past the
-- cut): the rows must be the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE hk_dim_c (id int PRIMARY KEY, enabled boolean);
INSERT INTO hk_dim_c SELECT g, g % 5 <> 0 FROM generate_series(1, 4740) g;
CREATE TABLE hk_entities (id int PRIMARY KEY, name text);
INSERT INTO hk_entities SELECT g, 'e' || g FROM generate_series(1, 500) g;
CREATE TABLE hk_links (scope_id int NOT NULL, dim_a_id int NOT NULL);
INSERT INTO hk_links VALUES (1, 11), (2, 7);
CREATE TABLE hk_events (id bigserial PRIMARY KEY, dim_a_id int, foreign_id int NOT NULL, day date NOT NULL, deleted_at timestamptz, metrics_a jsonb NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO hk_events (dim_a_id, foreign_id, day, metrics_a)
	SELECT e, 1 + (e + g) % 20, date '2025-06-01' + (g * 400 / 200), jsonb_build_object('mhead', 1 + (e * g) % 10, 'service_clean', 1 + (e + g) % 10)
	FROM generate_series(1, 500) e, generate_series(1, 200) g;
-- entity 7 owns a third of the table
INSERT INTO hk_events (dim_a_id, foreign_id, day, metrics_a)
	SELECT 7, 1 + g % 20, date '2025-06-01' + (g * 400 / 30000), jsonb_build_object('mhead', 1 + g % 10, 'service_clean', 1 + (7 + g) % 10) FROM generate_series(1, 30000) g;
CREATE INDEX hk_events_gen ON hk_events (dim_a_id, day, deleted_at);
CREATE INDEX hk_events_col ON hk_events USING sift_col (id, dim_a_id, foreign_id, day, deleted_at, metrics_a) WITH (key_cut = dim_a_id, paths = 'metrics_a.* float8', rollup_dims = 'off');
VACUUM (FREEZE, ANALYZE) hk_events, hk_dim_c, hk_entities, hk_links;

CREATE FUNCTION hk_q(sub int) RETURNS text LANGUAGE sql AS $$ SELECT format($f$
WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM hk_entities WHERE id IN (%s) AND id IN (SELECT dim_a_id FROM hk_links WHERE scope_id = %s)),
periods AS (SELECT * FROM (VALUES ('current', daterange('2025-09-01', '2026-03-01', '[]')), ('previous', daterange('2024-09-01', '2025-03-01', '[]'))) AS p(period, range)),
all_dates AS (SELECT period FROM periods),
ev AS (SELECT e.*, periods.period AS bucket_period FROM hk_events e INNER JOIN periods ON day <@ range INNER JOIN hk_entities n ON n.id = e.dim_a_id INNER JOIN ids ON ids.id = n.id
       INNER JOIN hk_dim_c s ON s.id = e.foreign_id AND s.enabled IS TRUE
       WHERE e.deleted_at IS NULL AND ((day >= '2025-09-01' AND day <= '2026-03-01') OR (day >= '2024-09-01' AND day <= '2025-03-01'))),
parsed AS (SELECT ev.id, ev.bucket_period, split_part(key, '_', 1) AS category, nullif(split_part(key, '_', 2), '') AS sub, (value #>> '{}')::float AS v FROM ev INNER JOIN LATERAL jsonb_each(metrics_a) ON true),
grouped AS (SELECT coalesce(category, 'mhead') AS category, sub, period, avg(v) AS value, coalesce(count(DISTINCT id), 0) AS n
            FROM (SELECT parsed.*, all_dates.period FROM all_dates LEFT JOIN parsed ON all_dates.period = parsed.bucket_period) x GROUP BY GROUPING SETS ((category, sub, period)))
SELECT category, sub, period, round(value::numeric, 6) AS value, n FROM grouped WHERE category = 'mhead' ORDER BY period, sub$f$, CASE sub WHEN 1 THEN 11 ELSE 7 END, sub) $$;

CREATE FUNCTION hk_plan(sub int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || hk_q(sub) INTO j;
	RETURN CASE WHEN j::text LIKE '%"SiftAgg"%' THEN 'SiftAgg' ELSE 'normal' END;
END $$;

CREATE FUNCTION hk_same(sub int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', hk_q(sub));
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', hk_q(sub));
	RESET sift.enable_agg;
	n := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN n;
END $$;

-- the plan taken: the small entity keeps the normal plan, the large one the scan
SELECT sub AS scope, hk_plan(sub) AS plan, hk_same(sub) AS rows_unlike_normal FROM generate_series(1, 2) sub;

-- updates, deletes, a NULL entity and a row past the cut between the runs
UPDATE hk_events SET metrics_a = metrics_a || '{"extra": 3}' WHERE id % 11 = 0 AND dim_a_id IN (7, 11);
DELETE FROM hk_events WHERE id % 13 = 0 AND dim_a_id IN (7, 11);
INSERT INTO hk_events (dim_a_id, foreign_id, day, metrics_a) VALUES (NULL, 3, date '2025-12-01', '{"mhead": 5}'), (11, 3, date '2025-12-01', '{"mhead": 9}'), (7, 4, date '2025-12-02', '{}');
SELECT sub AS scope, hk_same(sub) AS rows_unlike_normal FROM generate_series(1, 2) sub;

DROP FUNCTION hk_q(int), hk_plan(int), hk_same(int);
DROP TABLE hk_events, hk_links, hk_entities, hk_dim_c;
DROP EXTENSION pg_sift;
