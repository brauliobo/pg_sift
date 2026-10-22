-- A CTE that two aggregates read is planned inline when that puts the aggregates in the scan. Above the aggregates there can be a plan that costs
-- far more than they do (a FULL JOIN of the two on a key the planner cannot estimate, a window over its rows, 100 rows assumed for each call of a
-- set-returning function): it is the same in both plans, so it is not what decides the choice. Here the arms are a spine of buckets joined to the
-- rows of the CTE by a jsonb array of keys, the two documents of each row aggregated as two series, and the series combined with a window.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, metrics jsonb NOT NULL, payload jsonb);
INSERT INTO events
SELECT g, 1 + g % 50, date '2025-01-01' + (g * 7919 % 360), jsonb_build_object('p_total', (g % 9) + 1, 'asp_a', (g % 10) + 1, 'asp_b_x', (g * 3 % 10) + 1),
       CASE WHEN g % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || chr(97 + (g + j) % 4), 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 3) j)) END
FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, metrics, payload) WITH (paths = 'metrics.* float8, payload.parts[]{cat text, score float8}');
CREATE TABLE dims (id int PRIMARY KEY, kind text NOT NULL);
INSERT INTO dims SELECT g, 'cat_' || chr(97 + g % 4) FROM generate_series(1, 50) g;
VACUUM (FREEZE, ANALYZE) events;
ANALYZE dims;

CREATE FUNCTION series_query(cte text) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-06-01', DATE '2025-12-31', '[]'), interval '0 day'),
	                                                        ('prev', daterange(DATE '2024-06-01', DATE '2024-12-31', '[]'), interval '1 year')) p(period, range, shift)),
	  all_dates AS (SELECT period, generate_series(date_trunc('week', lower(range)::date)::date + shift, date_trunc('week', (upper(range) - 1)::date)::date + shift, interval '1 week')::date AS day FROM periods),
	  base AS %s (SELECT e.*, periods.period AS bucket_period, (date_trunc('week', e.ts)::date + periods.shift)::date AS bucket_date
	              FROM events e JOIN periods ON e.ts <@ periods.range JOIN dims ON dims.id = e.parent_id AND dims.kind <> 'cat_d'),
	  parts AS (SELECT b.id, b.parent_id, b.bucket_period, b.bucket_date, o.cat, avg(o.score) AS score
	            FROM base b CROSS JOIN LATERAL jsonb_to_recordset(b.payload['parts']) o(cat text, score float8) WHERE o.score IS NOT NULL
	            GROUP BY b.id, b.parent_id, b.bucket_period, b.bucket_date, o.cat),
	  metrics AS (SELECT b.id, b.parent_id, b.bucket_period, b.bucket_date, split_part(r.key, '_', 1) AS cat, r.value::float8 AS score
	              FROM base b CROSS JOIN LATERAL jsonb_each(b.metrics) r),
	  a AS (SELECT coalesce(cat, 'p_total') AS cat, day, period, avg(score) AS value, count(DISTINCT id) AS n
	        FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN parts x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
	        GROUP BY GROUPING SETS ((cat, day, period))),
	  b AS (SELECT coalesce(cat, 'p_total') AS cat, day, period, avg(score) AS value, count(DISTINCT id) AS n
	        FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN metrics x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
	        GROUP BY GROUPING SETS ((cat, day, period))),
	  g AS (SELECT coalesce(a.cat, b.cat) AS cat, coalesce(a.day, b.day) AS day, coalesce(a.period, b.period) AS period, coalesce(a.value, b.value) AS value,
	               coalesce(b.n, a.n) AS n, a.value AS a_value, b.value AS b_value
	        FROM a FULL JOIN b ON jsonb_build_array(a.cat, a.day, a.period) = jsonb_build_array(b.cat, b.day, b.period)),
	  t AS (SELECT g.*, lag(g.value) OVER w AS prev, lag(g.n) OVER w AS prev_n FROM g WINDOW w AS (PARTITION BY day, cat ORDER BY period DESC))
	SELECT cat, day, period, round(value::numeric, 6) AS value, n, round(a_value::numeric, 6) AS a_value, round(b_value::numeric, 6) AS b_value, round(prev::numeric, 6) AS prev, prev_n
	FROM t WHERE period = 'cur'$q$, cte)
$$;

-- the nodes of the scan in the plan, and the rows against the normal plan
CREATE FUNCTION scan_nodes(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP IF l ~ 'Custom Scan \(SiftAgg\)' THEN n := n + 1; END IF; END LOOP;
	RETURN n;
END $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	IF (SELECT count(*) FROM r_off) < 100 THEN RAISE EXCEPTION 'the series has few rows'; END IF;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- the CTE as PostgreSQL keeps it (read twice: materialized) and as written inline: both arms are scans, and the rows are the normal plan's
SELECT scan_nodes(series_query('')) AS shared_cte_nodes, scan_nodes(series_query('NOT MATERIALIZED')) AS inline_nodes;
SELECT differences(series_query('')) AS shared_cte_differences, differences(series_query('NOT MATERIALIZED')) AS inline_differences;
