-- The shapes of the plan-choice gate (scripts/plangate.sh) on the tables of gen.sql. Three lines add a shape: its name, its expectation and
-- the query (ending in `;`, and in a total ORDER BY when it has a LIMIT, so that every path returns the same rows):
--
--   -- name: a01_one_key
--   -- expect: scan 0.02 0.3        PLAN [LO HI]: the plan the planner picks at the default size (scan: Custom Scan (SiftAgg), normal, either)
--   SELECT ...;                     and the band of the planner's cost of the scan path over the normal one (`-` when there is no scan path)
--
-- `scripts/plangate.sh --suggest` prints the expectation each shape has now (the band is a factor of 2 around the measured ratio): read it, and keep
-- the plan only when the shape should have it. The families by the first letter: a filtered group by one to three keys, b top-N, c joined lookups,
-- d lateral lookups, e two-level aggregates, f spine joins, g EXISTS over elements, h partitions, i pre-aggregate answers, j count distinct,
-- k fused arms, l window over aggregates, m text search, n lookup lists (scope and skew), o other filters.

-- name: a01_one_key
-- expect: scan 0.17 0.66
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: a02_parent_key
-- expect: scan 0.17 0.67
SELECT parent_id, count(*) AS n, round(avg(amount)::numeric, 3) AS mean FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: a03_two_keys_month
-- expect: scan 0.18 0.73
SELECT kind, date_trunc('month', ts)::date AS bucket, sum(amount) AS total, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: a04_two_keys_parent_kind
-- expect: scan 0.17 0.66
SELECT parent_id, kind, count(*) AS n, min(ts) AS first_day, max(ts) AS last_day FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: a05_three_keys
-- expect: scan 0.18 0.74
SELECT parent_id, kind, date_trunc('quarter', ts)::date AS bucket, sum(qty) AS units, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;

-- name: a06_global
-- expect: scan 0.16 0.64
SELECT count(*) AS n, sum(amount) AS total, min(ts) AS first_day, max(ts) AS last_day FROM events
WHERE deleted_at IS NULL AND kind = 'cat_a' AND ts >= date '2024-03-01' AND ts < date '2025-03-01';

-- name: a07_filter_clauses
-- expect: scan 0.16 0.63
SELECT kind, count(*) FILTER (WHERE qty > 5) AS big, count(*) FILTER (WHERE qty <= 5) AS small, sum(amount) FILTER (WHERE qty > 5) AS big_total FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: a08_one_parent_narrow
-- expect: normal 2.0 8.1
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id = 7 AND deleted_at IS NULL AND ts >= date '2025-03-01' AND ts < date '2025-04-01' GROUP BY 1 ORDER BY 1;

-- name: a09_having
-- expect: either 0.45 1.8
SELECT parent_id, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 HAVING count(*) > 240 ORDER BY 1;

-- name: a10_expression_key
-- expect: scan 0.19 0.74
SELECT extract(isodow FROM ts)::int AS weekday, kind, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: a11_float_metric
-- expect: scan 0.23 0.92
SELECT kind, round(avg((doc -> 'metrics' ->> 'm_a')::float8)::numeric, 3) AS mean, round(sum((doc -> 'metrics' ->> 'm_a')::float8)::numeric, 2) AS total FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: a12_float_sum_unrounded
-- expect: scan 0.23 0.92
SELECT kind, sum((doc -> 'metrics' ->> 'm_a')::float8) AS total, avg((doc -> 'metrics' ->> 'm_a')::float8) AS mean FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: b01_top_groups
-- expect: scan 0.18 0.70
SELECT parent_id, sum(amount) AS total FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY total DESC, parent_id LIMIT 5;

-- name: b02_top_two_keys
-- expect: scan 0.17 0.67
SELECT parent_id, kind, count(*) AS n FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01'
GROUP BY 1, 2 ORDER BY n DESC, parent_id, kind LIMIT 10;

-- name: b03_top_rows_by_ts
-- expect: scan 0.16 0.66
SELECT id, ts, amount FROM events WHERE deleted_at IS NULL AND kind = 'cat_b' ORDER BY ts DESC, id DESC LIMIT 20;

-- name: b04_top_rows_one_parent
-- expect: normal 7.4 30
SELECT id, ts, amount FROM events WHERE parent_id = 7 AND deleted_at IS NULL ORDER BY ts DESC, id DESC LIMIT 10;

-- name: b05_top_months
-- expect: scan 0.16 0.64
SELECT date_trunc('month', ts)::date AS bucket, sum(amount) AS total FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY total DESC, bucket LIMIT 3;

-- name: b06_top_offset
-- expect: scan 0.29 1.2
SELECT parent_id, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY n DESC, parent_id LIMIT 5 OFFSET 5;

-- name: c01_join_tier
-- expect: scan 0.17 0.68
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events e JOIN dims d ON d.id = e.parent_id
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: c02_join_active_filter
-- expect: scan 0.17 0.67
SELECT d.tier, e.kind, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE d.active AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: c03_join_month
-- expect: scan 0.18 0.71
SELECT d.tier, date_trunc('month', e.ts)::date AS bucket, round(avg(e.amount)::numeric, 3) AS mean, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: c04_in_active_parents
-- expect: scan 0.17 0.69
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id IN (SELECT id FROM dims WHERE active) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: c05_join_two_lookups
-- expect: either 0.79 3.2
SELECT d.tier, count(*) AS n, max(f.mv) AS best FROM events e JOIN dims d ON d.id = e.parent_id
JOIN (SELECT parent_id, max(val) AS mv FROM facts GROUP BY 1) f ON f.parent_id = e.parent_id
WHERE f.mv > 10 AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: c06_join_spans_values
-- expect: scan 0.12 0.49
WITH spans AS (SELECT * FROM (VALUES ('span_a', daterange('2025-03-01', '2026-02-28', '[]')), ('span_b', daterange('2024-03-01', '2025-02-28', '[]'))) p(span, range))
SELECT p.span, e.kind, count(*) AS n, round(avg(e.amount)::numeric, 3) AS mean FROM events e JOIN spans p ON e.ts <@ p.range
WHERE e.parent_id IN (SELECT id FROM dims WHERE active) AND e.deleted_at IS NULL GROUP BY 1, 2 ORDER BY 1, 2;

-- name: c07_left_join_active
-- expect: scan 0.17 0.67
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events e LEFT JOIN dims d ON d.id = e.parent_id AND d.active
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: c08_join_other_fact_filter
-- expect: scan 0.26 1.0
SELECT d.tier, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE e.parent_id IN (SELECT parent_id FROM facts WHERE role = 'role_a' AND val > 10) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: c09_join_filter_on_lookup_column
-- expect: scan 0.16 0.66
SELECT e.kind, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE d.tier = 'tier_b' AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: d01_lateral_latest_fact
-- expect: either 0.47 1.9
SELECT e.kind, count(*) AS n, round(avg(l.val)::numeric, 3) AS mean FROM events e
LEFT JOIN LATERAL (SELECT f.val FROM facts f WHERE f.parent_id = e.parent_id AND f.ts <= e.ts ORDER BY f.ts DESC, f.id DESC LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2025-06-01' AND e.ts < date '2025-06-15' GROUP BY 1 ORDER BY 1;

-- name: d02_lateral_first_fact_per_parent
-- expect: scan 0.18 0.71
SELECT e.kind, count(*) AS n, sum(l.val) AS total FROM events e
LEFT JOIN LATERAL (SELECT f.val FROM facts f WHERE f.parent_id = e.parent_id ORDER BY f.ts, f.id LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: d03_lateral_recordset
-- expect: scan 0.042 0.17
SELECT p.kind, p.sub, round(avg(p.score)::numeric, 3) AS mean, count(*) AS n FROM events e
CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') AS p(kind text, sub text, sign text, score float8)
WHERE e.parent_id IN (SELECT id FROM dims WHERE active) AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: d04_lateral_series
-- expect: scan 0.0013 0.0052
SELECT e.kind, g.n AS copy, count(*) AS n FROM events e CROSS JOIN LATERAL generate_series(1, e.qty % 3 + 1) g(n)
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: d05_lateral_dim_limit_one
-- expect: scan 0.13 0.54
SELECT l.tier, count(*) AS n, sum(e.amount) AS total FROM events e
LEFT JOIN LATERAL (SELECT d.tier FROM dims d WHERE d.id = e.parent_id ORDER BY d.id LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: e01_mean_of_counts
-- expect: scan 0.18 0.70
SELECT kind, round(avg(n), 2) AS mean_n, max(n) AS max_n FROM (SELECT parent_id, kind, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s GROUP BY 1 ORDER BY 1;

-- name: e02_sum_of_maxima_by_month
-- expect: scan 0.17 0.68
SELECT bucket, sum(m) AS total FROM (SELECT parent_id, date_trunc('month', ts)::date AS bucket, max(amount) AS m FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s GROUP BY 1 ORDER BY 1;

-- name: e03_mean_of_means
-- expect: scan 0.18 0.71
SELECT kind, avg(mean_amount) AS mean FROM (SELECT parent_id, kind, avg(amount) AS mean_amount FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s GROUP BY 1 ORDER BY 1;

-- name: e04_groups_over_threshold
-- expect: scan 0.18 0.71
SELECT kind, count(*) AS parents FROM (SELECT parent_id, kind, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1, 2 HAVING count(*) > 100) s GROUP BY 1 ORDER BY 1;

-- name: e05_two_level_join_outer
-- expect: scan 0.29 1.2
SELECT d.tier, sum(s.n) AS n FROM (SELECT parent_id, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1) s JOIN dims d ON d.id = s.parent_id GROUP BY 1 ORDER BY 1;

-- name: e06_count_of_groups
-- expect: scan 0.18 0.70
SELECT count(*) AS cells FROM (SELECT parent_id, kind FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s;

-- name: f01_spine_range_join
-- expect: scan 0.13 0.52
SELECT b.bucket, count(e.id) AS n, sum(e.amount) AS total FROM buckets b
LEFT JOIN events e ON e.ts >= b.bucket AND e.ts < b.bucket + interval '1 month' AND e.deleted_at IS NULL AND e.kind = 'cat_a' GROUP BY 1 ORDER BY 1;

-- name: f02_spine_derived_cells
-- expect: scan 0.29 1.1
SELECT b.bucket, coalesce(e.n, 0) AS n, coalesce(e.total, 0) AS total FROM buckets b
LEFT JOIN (SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n, sum(amount) AS total FROM events WHERE deleted_at IS NULL AND parent_id = 7 GROUP BY 1) e ON e.bucket = b.bucket
ORDER BY 1;

-- name: f03_spine_expression_key
-- expect: scan 0.17 0.70
SELECT b.bucket, count(e.id) AS n FROM buckets b LEFT JOIN events e ON date_trunc('month', e.ts)::date = b.bucket AND e.kind = 'cat_a' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: f04_spine_span_kind
-- expect: normal -
SELECT b.span, e.kind, count(*) AS n, sum(e.amount) AS total FROM buckets b JOIN events e ON e.ts >= b.bucket AND e.ts < b.bucket + interval '1 month'
WHERE e.deleted_at IS NULL GROUP BY 1, 2 ORDER BY 1, 2;

-- name: f05_spine_series
-- expect: scan 0.16 0.62
SELECT g.bucket::date AS bucket, count(e.id) AS n FROM generate_series(date '2024-01-01', date '2025-12-01', interval '1 month') g(bucket)
LEFT JOIN events e ON e.ts >= g.bucket AND e.ts < g.bucket + interval '1 month' AND e.deleted_at IS NULL AND e.parent_id IN (SELECT id FROM dims WHERE tier = 'tier_a') GROUP BY 1 ORDER BY 1;

-- name: f06_spine_dims_cross
-- expect: normal -
SELECT d.tier, b.span, count(e.id) AS n FROM dims d CROSS JOIN (SELECT DISTINCT span FROM buckets) b
LEFT JOIN events e ON e.parent_id = d.id AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' AND (e.ts >= date '2025-01-01') = (b.span = 'span_a')
GROUP BY 1, 2 ORDER BY 1, 2;

-- name: g01_exists_element
-- expect: scan 0.0075 0.030
SELECT kind, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(kind text, sign text) WHERE p.sign = 'neg' AND p.kind = 'cat_a') AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: g02_not_exists_element
-- expect: scan 0.0083 0.033
SELECT kind, count(*) AS n FROM events e
WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(kind text, sign text) WHERE p.sign = 'neg') AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: g03_jsonpath_element
-- expect: scan 0.19 0.76
SELECT kind, count(*) AS n FROM events e WHERE e.doc @? '$.parts[*] ? (@.sign == "neg" && @.kind == "cat_b")' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: g04_exists_by_parent_month
-- expect: scan 0.016 0.065
SELECT parent_id, date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(sub text, score float8) WHERE p.sub = 'asp_a' AND p.score >= 5)
AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: g05_exists_in_table
-- expect: scan 0.16 0.65
SELECT e.kind, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM facts f WHERE f.parent_id = e.parent_id AND f.role = 'role_a' AND f.val > 10) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: g06_not_exists_in_table
-- expect: scan 0.11 0.42
SELECT e.kind, count(*) AS n FROM events e
WHERE NOT EXISTS (SELECT 1 FROM facts f WHERE f.parent_id = e.parent_id AND f.val > 11) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: g07_elements_counted
-- expect: scan 0.063 0.25
SELECT p.kind, count(*) AS n FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') p(kind text, score float8)
WHERE p.score > 5 AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: h01_partitions_group
-- expect: either 0.37 1.5
SELECT kind, count(*) AS n, sum(amount) AS total FROM events_part WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: h02_partition_pruned
-- expect: normal -
SELECT parent_id, count(*) AS n FROM events_part WHERE deleted_at IS NULL AND ts >= date '2025-02-01' AND ts < date '2025-04-01' GROUP BY 1 ORDER BY 1;

-- name: h03_partitions_join
-- expect: scan 0.36 1.4
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events_part e JOIN dims d ON d.id = e.parent_id WHERE e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: h04_partitions_top
-- expect: either 0.39 1.6
SELECT parent_id, sum(amount) AS total FROM events_part WHERE deleted_at IS NULL GROUP BY 1 ORDER BY total DESC, parent_id LIMIT 5;

-- name: h05_partitions_count
-- expect: either 0.35 1.4
SELECT count(*) AS n FROM events_part;

-- name: i01_count_all
-- expect: scan 0.31 1.2
SELECT count(*) AS n FROM events;

-- name: i02_count_by_kind
-- expect: scan 0.15 0.58
SELECT kind, count(*) AS n FROM events GROUP BY 1 ORDER BY 1;

-- name: i03_sum_by_parent
-- expect: scan 0.17 0.69
SELECT parent_id, sum(amount) AS total FROM events GROUP BY 1 ORDER BY 1;

-- name: i04_min_max
-- expect: scan 0.17 0.68
SELECT min(ts) AS first_day, max(ts) AS last_day, min(amount) AS low, max(amount) AS high FROM events;

-- name: i05_count_live
-- expect: scan 0.28 1.1
SELECT count(*) AS n FROM events WHERE deleted_at IS NULL;

-- name: i06_sum_by_kind_month
-- expect: scan 0.18 0.73
SELECT kind, date_trunc('month', ts)::date AS bucket, sum(amount) AS total, count(*) AS n FROM events GROUP BY 1, 2 ORDER BY 1, 2;

-- name: j01_distinct_global
-- expect: scan 0.31 1.2
SELECT count(DISTINCT parent_id) AS parents FROM events WHERE deleted_at IS NULL;

-- name: j02_distinct_by_kind
-- expect: scan 0.10 0.40
SELECT kind, count(DISTINCT parent_id) AS parents FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: j03_distinct_by_month
-- expect: scan 0.096 0.38
SELECT date_trunc('month', ts)::date AS bucket, count(DISTINCT parent_id) AS parents FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: j04_two_distincts
-- expect: scan 0.081 0.33
SELECT kind, count(DISTINCT parent_id) AS parents, count(DISTINCT ts) AS days, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: j05_distinct_one_parent
-- expect: scan 0.28 1.1
SELECT kind, count(DISTINCT ts) AS days FROM events WHERE parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: j06_select_distinct
-- expect: scan 0.17 0.67
SELECT DISTINCT parent_id, kind FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' ORDER BY 1, 2;

-- name: j07_distinct_join
-- expect: scan 0.099 0.40
SELECT d.tier, count(DISTINCT e.parent_id) AS parents, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id WHERE e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: k01_arms_union
-- expect: scan 0.25 1.00
SELECT 'asp_a' AS arm, round(avg((doc -> 'metrics' ->> 'asp_a')::float8)::numeric, 3) AS mean, count(*) AS n FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01'
UNION ALL
SELECT 'asp_b', round(avg((doc -> 'metrics' ->> 'asp_b')::float8)::numeric, 3), count(*) FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' ORDER BY 1;

-- name: k02_arms_scalar_subselects
-- expect: scan 0.19 0.77
SELECT (SELECT avg(amount) FROM events WHERE kind = 'cat_a' AND deleted_at IS NULL) AS mean_a, (SELECT avg(amount) FROM events WHERE kind = 'cat_b' AND deleted_at IS NULL) AS mean_b,
       (SELECT count(*) FROM events WHERE deleted_at IS NULL) AS n;

-- name: k03_arms_filter
-- expect: scan 0.16 0.64
SELECT parent_id, avg(amount) FILTER (WHERE kind = 'cat_a') AS mean_a, avg(amount) FILTER (WHERE kind = 'cat_b') AS mean_b, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: k04_arms_per_parent_metrics
-- expect: scan 0.32 1.3
SELECT parent_id, round(avg((doc -> 'metrics' ->> 'asp_a')::float8)::numeric, 3) AS asp_a, round(avg((doc -> 'metrics' ->> 'asp_b')::float8)::numeric, 3) AS asp_b, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: l01_rank_within_month
-- expect: scan 0.21 0.83
SELECT bucket, kind, n FROM (SELECT bucket, kind, n, rank() OVER (PARTITION BY bucket ORDER BY n DESC, kind) AS rk
	FROM (SELECT date_trunc('month', ts)::date AS bucket, kind, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s) w WHERE rk <= 2 ORDER BY 1, 2;

-- name: l02_running_total
-- expect: scan 0.17 0.67
SELECT bucket, sum(amount) AS total, sum(sum(amount)) OVER (ORDER BY bucket) AS running FROM (SELECT date_trunc('month', ts)::date AS bucket, amount FROM events WHERE deleted_at IS NULL) s GROUP BY 1 ORDER BY 1;

-- name: l03_share_of_total
-- expect: scan 0.18 0.70
SELECT kind, sum(amount) AS total, round((100 * sum(amount) / sum(sum(amount)) OVER ())::numeric, 2) AS pct FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: l04_lag_of_months
-- expect: scan 0.17 0.69
SELECT bucket, n, n - lag(n) OVER (ORDER BY bucket) AS change FROM (SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE deleted_at IS NULL AND kind = 'cat_a' GROUP BY 1) s ORDER BY 1;

-- name: l05_top_parents_per_kind
-- expect: scan 0.18 0.72
SELECT kind, parent_id, total FROM (SELECT kind, parent_id, total, row_number() OVER (PARTITION BY kind ORDER BY total DESC, parent_id) AS rn
	FROM (SELECT kind, parent_id, sum(amount) AS total FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s) w WHERE rn <= 3 ORDER BY 1, 3 DESC, 2;

-- name: m01_search_group
-- expect: scan 0.19 0.75
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE memo @@> 'cable wire' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: m02_search_count
-- expect: normal -
SELECT count(*) AS n FROM events WHERE memo @@> 'cable';

-- name: m03_search_one_parent
-- expect: either 0.41 1.6
SELECT kind, count(*) AS n FROM events WHERE memo @@> 'switch' AND parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: m04_search_latest_rows
-- expect: scan 0.19 0.76
SELECT id, ts, amount FROM events WHERE memo @@> 'cable wire' AND deleted_at IS NULL ORDER BY ts DESC, id DESC LIMIT 8;

-- name: m05_search_or_by_month
-- expect: scan 0.19 0.76
SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE memo @@> 'cable OR relay' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: m06_search_join
-- expect: scan 0.20 0.80
SELECT d.tier, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id WHERE e.memo @@> 'plug' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: n01_scope_path_filter_in
-- expect: either 4.1 16
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id IN (SELECT d.id FROM dims d WHERE d.settings ->> 'tier' = 'tier_b') AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: n02_scope_path_filter_join
-- expect: either 4.1 16
SELECT e.kind, count(*) AS n, sum(e.amount) AS total FROM events e JOIN dims d ON d.id = e.parent_id
WHERE d.settings ->> 'tier' = 'tier_c' AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: n03_scope_expression_filter
-- expect: either 4.0 16
SELECT kind, count(*) AS n FROM events WHERE parent_id IN (SELECT d.id FROM dims d WHERE d.id % 3 = 1) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: n04_skew_heaviest_parents
-- expect: scan 0.17 0.67
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE skew_id IN (SELECT id FROM dims WHERE id <= 5) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: n05_skew_rarest_parents
-- expect: scan 0.17 0.67
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE skew_id IN (SELECT id FROM dims WHERE id > 55) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: n06_flat_list
-- expect: scan 0.17 0.67
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (SELECT id FROM dims WHERE id <= 5) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: o01_any_array_subselect
-- expect: scan 0.18 0.73
SELECT kind, count(*) AS n FROM events WHERE parent_id = ANY (ARRAY (SELECT id FROM dims WHERE active AND id % 5 = 0)) AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: o02_in_constants
-- expect: scan 0.19 0.77
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (1, 2, 3, 4, 5, 6, 7, 8) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: o03_range_only_by_month
-- expect: either 0.40 1.6
SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: o04_one_day
-- expect: normal 1.7 6.9
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE ts = date '2025-03-01' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: o05_deleted_rows
-- expect: scan 0.16 0.66
SELECT kind, count(*) AS n FROM events WHERE deleted_at IS NOT NULL GROUP BY 1 ORDER BY 1;

-- name: o06_text_not_equal
-- expect: scan 0.16 0.65
SELECT parent_id, count(*) AS n FROM events WHERE kind <> 'cat_a' AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;
