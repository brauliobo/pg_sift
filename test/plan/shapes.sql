-- The shapes of the plan-choice gate (scripts/plangate.sh) on the tables of gen.sql. Three lines add a shape: its name, its expectation and
-- the query (ending in `;`, and in a total ORDER BY when it has a LIMIT, so that every path returns the same rows):
--
--   -- name: one_key
--   -- expect: scan 0.02 0.3        PLAN [LO HI]: the plan the planner picks at the default size (scan: Custom Scan (SiftAgg), normal, either)
--   SELECT ...;                     and the band of the planner's cost of the scan path over the normal one (`-` when there is no scan path)
--
-- `scripts/plangate.sh --suggest` prints the expectation each shape has now (the band is a factor of 2 around the measured ratio): read it, and keep
-- the plan only when the shape should have it. The shapes come in families, in this order: a filtered group by one to three keys, top-N, joined lookups,
-- lateral lookups, two-level aggregates, spine joins, EXISTS over elements, partitions, pre-aggregate answers, count distinct, fused arms, window over
-- aggregates, text search, lookup lists (scope and skew), other filters.

-- name: one_key
-- expect: scan 0.10 0.34
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: parent_key
-- expect: scan 0.10 0.34
SELECT parent_id, count(*) AS n, round(avg(amount)::numeric, 3) AS mean FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: two_keys_month
-- expect: scan 0.081 0.32
SELECT kind, date_trunc('month', ts)::date AS bucket, sum(amount) AS total, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: two_keys_parent_kind
-- expect: scan 0.11 0.34
SELECT parent_id, kind, count(*) AS n, min(ts) AS first_day, max(ts) AS last_day FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: three_keys
-- expect: scan 0.089 0.36
SELECT parent_id, kind, date_trunc('quarter', ts)::date AS bucket, sum(qty) AS units, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;

-- name: global
-- expect: scan 0.050 0.20
SELECT count(*) AS n, sum(amount) AS total, min(ts) AS first_day, max(ts) AS last_day FROM events
WHERE deleted_at IS NULL AND kind = 'cat_a' AND ts >= date '2024-03-01' AND ts < date '2025-03-01';

-- name: filter_clauses
-- expect: scan 0.10 0.32
SELECT kind, count(*) FILTER (WHERE qty > 5) AS big, count(*) FILTER (WHERE qty <= 5) AS small, sum(amount) FILTER (WHERE qty > 5) AS big_total FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: one_parent_narrow
-- expect: either 0.49 2.0
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id = 7 AND deleted_at IS NULL AND ts >= date '2025-03-01' AND ts < date '2025-04-01' GROUP BY 1 ORDER BY 1;

-- name: having
-- expect: either 0.19 0.77
SELECT parent_id, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 HAVING count(*) > 240 ORDER BY 1;

-- name: expression_key
-- expect: scan 0.079 0.32
SELECT extract(isodow FROM ts)::int AS weekday, kind, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: float_metric
-- expect: scan 0.083 0.33
SELECT kind, round(avg((doc -> 'metrics' ->> 'm_a')::float8)::numeric, 3) AS mean, round(sum((doc -> 'metrics' ->> 'm_a')::float8)::numeric, 2) AS total FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: float_sum_unrounded
-- expect: scan 0.083 0.33
SELECT kind, sum((doc -> 'metrics' ->> 'm_a')::float8) AS total, avg((doc -> 'metrics' ->> 'm_a')::float8) AS mean FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: top_groups
-- expect: scan 0.11 0.35
SELECT parent_id, sum(amount) AS total FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY total DESC, parent_id LIMIT 5;

-- name: top_two_keys
-- expect: scan 0.10 0.34
SELECT parent_id, kind, count(*) AS n FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01'
GROUP BY 1, 2 ORDER BY n DESC, parent_id, kind LIMIT 10;

-- name: top_rows_by_ts
-- expect: scan 0.096 0.34
SELECT id, ts, amount FROM events WHERE deleted_at IS NULL AND kind = 'cat_b' ORDER BY ts DESC, id DESC LIMIT 20;

-- name: top_rows_one_parent
-- expect: normal 5.7 9.7
SELECT id, ts, amount FROM events WHERE parent_id = 7 AND deleted_at IS NULL ORDER BY ts DESC, id DESC LIMIT 10;

-- name: top_months
-- expect: scan 0.099 0.31
SELECT date_trunc('month', ts)::date AS bucket, sum(amount) AS total FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY total DESC, bucket LIMIT 3;

-- name: top_offset
-- expect: scan 0.12 0.49
SELECT parent_id, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY n DESC, parent_id LIMIT 5 OFFSET 5;

-- name: top_rows_trailing_key
-- expect: scan 0.073 0.26
SELECT id, ts, memo FROM events WHERE deleted_at IS NULL ORDER BY ts, memo, id LIMIT 10;

-- name: top_rows_trailing_key_desc_offset
-- expect: scan 0.091 0.34
SELECT id, ts, memo FROM events WHERE deleted_at IS NULL AND kind = 'cat_b' ORDER BY ts DESC, memo DESC, id LIMIT 20 OFFSET 15;

-- name: top_rows_trailing_key_ties
-- expect: normal -
SELECT id, kind, memo FROM events ORDER BY kind, memo, id LIMIT 10;

-- name: join_tier
-- expect: scan 0.11 0.35
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events e JOIN dims d ON d.id = e.parent_id
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: join_active_filter
-- expect: scan 0.11 0.34
SELECT d.tier, e.kind, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE d.active AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: join_month
-- expect: scan 0.087 0.35
SELECT d.tier, date_trunc('month', e.ts)::date AS bucket, round(avg(e.amount)::numeric, 3) AS mean, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: in_active_parents
-- expect: scan 0.12 0.37
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id IN (SELECT id FROM dims WHERE active) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: in_expr_key
-- expect: scan 0.12 0.48
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id IN (SELECT id + 1 FROM dims WHERE active) AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: join_two_lookups
-- expect: either 0.79 3.2
SELECT d.tier, count(*) AS n, max(f.mv) AS best FROM events e JOIN dims d ON d.id = e.parent_id
JOIN (SELECT parent_id, max(val) AS mv FROM facts GROUP BY 1) f ON f.parent_id = e.parent_id
WHERE f.mv > 10 AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: join_spans_values
-- expect: scan 0.076 0.25
WITH spans AS (SELECT * FROM (VALUES ('span_a', daterange('2025-03-01', '2026-02-28', '[]')), ('span_b', daterange('2024-03-01', '2025-02-28', '[]'))) p(span, range))
SELECT p.span, e.kind, count(*) AS n, round(avg(e.amount)::numeric, 3) AS mean FROM events e JOIN spans p ON e.ts <@ p.range
WHERE e.parent_id IN (SELECT id FROM dims WHERE active) AND e.deleted_at IS NULL GROUP BY 1, 2 ORDER BY 1, 2;

-- name: left_join_active
-- expect: scan 0.11 0.34
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events e LEFT JOIN dims d ON d.id = e.parent_id AND d.active
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: left_cross_dims
-- expect: scan 0.07 0.27
SELECT k.kind, s.skew_id, count(e.id) AS n, sum(e.qty) AS q FROM (SELECT DISTINCT kind FROM events) k CROSS JOIN (SELECT DISTINCT skew_id FROM events WHERE skew_id <= 5) s
LEFT JOIN events e ON e.kind = k.kind AND e.skew_id = s.skew_id GROUP BY 1, 2 ORDER BY 1, 2;

-- name: join_other_fact_filter
-- expect: scan 0.23 0.93
SELECT d.tier, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE e.parent_id IN (SELECT parent_id FROM facts WHERE role = 'type_a' AND val > 10) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: join_filter_on_lookup_column
-- expect: scan 0.10 0.34
SELECT e.kind, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id
WHERE d.tier = 'tier_b' AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: lateral_latest_fact
-- expect: either 0.34 0.65
SELECT e.kind, count(*) AS n, round(avg(l.val)::numeric, 3) AS mean FROM events e
LEFT JOIN LATERAL (SELECT f.val FROM facts f WHERE f.parent_id = e.parent_id AND f.ts <= e.ts ORDER BY f.ts DESC, f.id DESC LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2025-06-01' AND e.ts < date '2025-06-15' GROUP BY 1 ORDER BY 1;

-- name: lateral_first_fact_per_parent
-- expect: scan 0.13 0.34
SELECT e.kind, count(*) AS n, sum(l.val) AS total FROM events e
LEFT JOIN LATERAL (SELECT f.val FROM facts f WHERE f.parent_id = e.parent_id ORDER BY f.ts, f.id LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: lateral_recordset
-- expect: scan 0.042 0.17
SELECT p.kind, p.sub, round(avg(p.score)::numeric, 3) AS mean, count(*) AS n FROM events e
CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') AS p(kind text, sub text, dir_a text, score float8)
WHERE e.parent_id IN (SELECT id FROM dims WHERE active) AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: lateral_series
-- expect: scan 0.00077 0.0026
SELECT e.kind, g.n AS copy, count(*) AS n FROM events e CROSS JOIN LATERAL generate_series(1, e.qty % 3 + 1) g(n)
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: lateral_dim_limit_one
-- expect: scan 0.088 0.27
SELECT l.tier, count(*) AS n, sum(e.amount) AS total FROM events e
LEFT JOIN LATERAL (SELECT d.tier FROM dims d WHERE d.id = e.parent_id ORDER BY d.id LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: mean_of_counts
-- expect: scan 0.11 0.37
SELECT kind, round(avg(n), 2) AS mean_n, max(n) AS max_n FROM (SELECT parent_id, kind, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s GROUP BY 1 ORDER BY 1;

-- name: sum_of_maxima_by_month
-- expect: scan 0.11 0.35
SELECT bucket, sum(m) AS total FROM (SELECT parent_id, date_trunc('month', ts)::date AS bucket, max(amount) AS m FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s GROUP BY 1 ORDER BY 1;

-- name: mean_of_means
-- expect: scan 0.12 0.37
SELECT kind, avg(mean_amount) AS mean FROM (SELECT parent_id, kind, avg(amount) AS mean_amount FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s GROUP BY 1 ORDER BY 1;

-- name: groups_over_threshold
-- expect: scan 0.11 0.37
SELECT kind, count(*) AS parents FROM (SELECT parent_id, kind, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1, 2 HAVING count(*) > 100) s GROUP BY 1 ORDER BY 1;

-- name: two_level_join_outer
-- expect: scan 0.13 0.50
SELECT d.tier, sum(s.n) AS n FROM (SELECT parent_id, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1) s JOIN dims d ON d.id = s.parent_id GROUP BY 1 ORDER BY 1;

-- name: count_of_groups
-- expect: scan 0.11 0.36
SELECT count(*) AS cells FROM (SELECT parent_id, kind FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s;

-- name: spine_range_join
-- expect: scan 0.13 0.52
SELECT b.bucket, count(e.id) AS n, sum(e.amount) AS total FROM buckets b
LEFT JOIN events e ON e.ts >= b.bucket AND e.ts < b.bucket + interval '1 month' AND e.deleted_at IS NULL AND e.kind = 'cat_a' GROUP BY 1 ORDER BY 1;

-- name: spine_derived_cells
-- expect: scan 0.068 0.27
SELECT b.bucket, coalesce(e.n, 0) AS n, coalesce(e.total, 0) AS total FROM buckets b
LEFT JOIN (SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n, sum(amount) AS total FROM events WHERE deleted_at IS NULL AND parent_id = 7 GROUP BY 1) e ON e.bucket = b.bucket
ORDER BY 1;

-- name: spine_expression_key
-- expect: scan 0.084 0.34
SELECT b.bucket, count(e.id) AS n FROM buckets b LEFT JOIN events e ON date_trunc('month', e.ts)::date = b.bucket AND e.kind = 'cat_a' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: spine_span_kind
-- expect: scan 0.15 0.60
SELECT b.span, e.kind, count(*) AS n, sum(e.amount) AS total FROM buckets b JOIN events e ON e.ts >= b.bucket AND e.ts < b.bucket + interval '1 month'
WHERE e.deleted_at IS NULL GROUP BY 1, 2 ORDER BY 1, 2;

-- name: spine_series
-- expect: scan 0.070 0.28
SELECT g.bucket::date AS bucket, count(e.id) AS n FROM generate_series(date '2024-01-01', date '2025-12-01', interval '1 month') g(bucket)
LEFT JOIN events e ON e.ts >= g.bucket AND e.ts < g.bucket + interval '1 month' AND e.deleted_at IS NULL AND e.parent_id IN (SELECT id FROM dims WHERE tier = 'tier_a') GROUP BY 1 ORDER BY 1;

-- name: spine_dims_cross
-- expect: normal -
SELECT d.tier, b.span, count(e.id) AS n FROM dims d CROSS JOIN (SELECT DISTINCT span FROM buckets) b
LEFT JOIN events e ON e.parent_id = d.id AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' AND (e.ts >= date '2025-01-01') = (b.span = 'span_a')
GROUP BY 1, 2 ORDER BY 1, 2;

-- name: exists_element
-- expect: scan 0.0053 0.016
SELECT kind, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(kind text, dir_a text) WHERE p.dir_a = 'neg' AND p.kind = 'cat_a') AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: not_exists_element
-- expect: scan 0.0053 0.017
SELECT kind, count(*) AS n FROM events e
WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(kind text, dir_a text) WHERE p.dir_a = 'neg') AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: jsonpath_element
-- expect: scan 0.13 0.39
SELECT kind, count(*) AS n FROM events e WHERE e.doc @? '$.parts[*] ? (@.dir_a == "neg" && @.kind == "cat_b")' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: exists_by_parent_month
-- expect: scan 0.0096 0.028
SELECT parent_id, date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(sub text, score float8) WHERE p.sub = 'catg_a' AND p.score >= 5)
AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1, 2 ORDER BY 1, 2;

-- name: exists_in_table
-- expect: scan 0.14 0.57
SELECT e.kind, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM facts f WHERE f.parent_id = e.parent_id AND f.role = 'type_a' AND f.val > 10) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: not_exists_in_table
-- expect: scan 0.091 0.36
SELECT e.kind, count(*) AS n FROM events e
WHERE NOT EXISTS (SELECT 1 FROM facts f WHERE f.parent_id = e.parent_id AND f.val > 11) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: elements_counted
-- expect: scan 0.063 0.25
SELECT p.kind, count(*) AS n FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') p(kind text, score float8)
WHERE p.score > 5 AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: partitions_group
-- expect: either 0.24 0.74
SELECT kind, count(*) AS n, sum(amount) AS total FROM events_part WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: partition_pruned
-- expect: normal -
SELECT parent_id, count(*) AS n FROM events_part WHERE deleted_at IS NULL AND ts >= date '2025-02-01' AND ts < date '2025-04-01' GROUP BY 1 ORDER BY 1;

-- name: partitions_join
-- expect: scan 0.25 0.73
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events_part e JOIN dims d ON d.id = e.parent_id WHERE e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: partitions_top
-- expect: either 0.24 0.78
SELECT parent_id, sum(amount) AS total FROM events_part WHERE deleted_at IS NULL GROUP BY 1 ORDER BY total DESC, parent_id LIMIT 5;

-- name: partitions_count
-- expect: either 0.068 0.27
SELECT count(*) AS n FROM events_part;

-- name: count_all
-- expect: scan 0.059 0.24
SELECT count(*) AS n FROM events;

-- name: count_by_kind
-- expect: scan 0.070 0.28
SELECT kind, count(*) AS n FROM events GROUP BY 1 ORDER BY 1;

-- name: sum_by_parent
-- expect: scan 0.10 0.35
SELECT parent_id, sum(amount) AS total FROM events GROUP BY 1 ORDER BY 1;

-- name: min_max
-- expect: scan 0.061 0.25
SELECT min(ts) AS first_day, max(ts) AS last_day, min(amount) AS low, max(amount) AS high FROM events;

-- name: count_live
-- expect: scan 0.055 0.22
SELECT count(*) AS n FROM events WHERE deleted_at IS NULL;

-- name: sum_by_kind_month
-- expect: scan 0.082 0.33
SELECT kind, date_trunc('month', ts)::date AS bucket, sum(amount) AS total, count(*) AS n FROM events GROUP BY 1, 2 ORDER BY 1, 2;

-- name: distinct_global
-- expect: scan 0.051 0.20
SELECT count(DISTINCT parent_id) AS parents FROM events WHERE deleted_at IS NULL;

-- name: distinct_by_kind
-- expect: scan 0.061 0.21
SELECT kind, count(DISTINCT parent_id) AS parents FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: distinct_by_month
-- expect: scan 0.054 0.19
SELECT date_trunc('month', ts)::date AS bucket, count(DISTINCT parent_id) AS parents FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: two_distincts
-- expect: scan 0.054 0.17
SELECT kind, count(DISTINCT parent_id) AS parents, count(DISTINCT ts) AS days, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: distinct_one_parent
-- expect: scan 0.066 0.26
SELECT kind, count(DISTINCT ts) AS days FROM events WHERE parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: select_distinct
-- expect: scan 0.10 0.34
SELECT DISTINCT parent_id, kind FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' ORDER BY 1, 2;

-- name: distinct_join
-- expect: scan 0.068 0.21
SELECT d.tier, count(DISTINCT e.parent_id) AS parents, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id WHERE e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: arms_union
-- expect: scan 0.051 0.20
SELECT 'catg_a' AS arm, round(avg((doc -> 'metrics' ->> 'catg_a')::float8)::numeric, 3) AS mean, count(*) AS n FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01'
UNION ALL
SELECT 'catg_b', round(avg((doc -> 'metrics' ->> 'catg_b')::float8)::numeric, 3), count(*) FROM events WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' ORDER BY 1;

-- name: arms_scalar_subselects
-- expect: scan 0.048 0.19
SELECT (SELECT avg(amount) FROM events WHERE kind = 'cat_a' AND deleted_at IS NULL) AS mean_a, (SELECT avg(amount) FROM events WHERE kind = 'cat_b' AND deleted_at IS NULL) AS mean_b,
       (SELECT count(*) FROM events WHERE deleted_at IS NULL) AS n;

-- name: arms_filter
-- expect: scan 0.10 0.34
SELECT parent_id, avg(amount) FILTER (WHERE kind = 'cat_a') AS mean_a, avg(amount) FILTER (WHERE kind = 'cat_b') AS mean_b, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: arms_per_parent_metrics
-- expect: scan 0.092 0.37
SELECT parent_id, round(avg((doc -> 'metrics' ->> 'catg_a')::float8)::numeric, 3) AS catg_a, round(avg((doc -> 'metrics' ->> 'catg_b')::float8)::numeric, 3) AS catg_b, count(*) AS n FROM events
WHERE deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: rank_within_month
-- expect: scan 0.096 0.5
SELECT bucket, kind, n FROM (SELECT bucket, kind, n, rank() OVER (PARTITION BY bucket ORDER BY n DESC, kind) AS rk
	FROM (SELECT date_trunc('month', ts)::date AS bucket, kind, count(*) AS n FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s) w WHERE rk <= 2 ORDER BY 1, 2;

-- name: running_total
-- expect: scan 0.096 0.30
SELECT bucket, sum(amount) AS total, sum(sum(amount)) OVER (ORDER BY bucket) AS running FROM (SELECT date_trunc('month', ts)::date AS bucket, amount FROM events WHERE deleted_at IS NULL) s GROUP BY 1 ORDER BY 1;

-- name: share_of_total
-- expect: scan 0.11 0.35
SELECT kind, sum(amount) AS total, round((100 * sum(amount) / sum(sum(amount)) OVER ())::numeric, 2) AS pct FROM events WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: lag_of_months
-- expect: scan 0.084 0.34
SELECT bucket, n, n - lag(n) OVER (ORDER BY bucket) AS change FROM (SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE deleted_at IS NULL AND kind = 'cat_a' GROUP BY 1) s ORDER BY 1;

-- name: top_parents_per_kind
-- expect: scan 0.12 0.37
SELECT kind, parent_id, total FROM (SELECT kind, parent_id, total, row_number() OVER (PARTITION BY kind ORDER BY total DESC, parent_id) AS rn
	FROM (SELECT kind, parent_id, sum(amount) AS total FROM events WHERE deleted_at IS NULL GROUP BY 1, 2) s) w WHERE rn <= 3 ORDER BY 1, 3 DESC, 2;

-- name: top_rows_per_kind_unindexed_column
-- expect: normal -
SELECT id, kind, memo, rn FROM (SELECT id, kind, memo, row_number() OVER (PARTITION BY kind ORDER BY amount DESC, id) AS rn FROM events WHERE deleted_at IS NULL) w WHERE rn <= 3 ORDER BY 2, 4;

-- name: search_group
-- expect: scan 0.093 0.50
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE memo @@> 'cable wire' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: search_count
-- expect: normal -
SELECT count(*) AS n FROM events WHERE memo @@> 'cable';

-- name: search_one_parent
-- expect: either 0.20 0.81
SELECT kind, count(*) AS n FROM events WHERE memo @@> 'switch' AND parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: search_latest_rows
-- expect: scan 0.095 0.50
SELECT id, ts, amount FROM events WHERE memo @@> 'cable wire' AND deleted_at IS NULL ORDER BY ts DESC, id DESC LIMIT 8;

-- name: search_or_by_month
-- expect: scan 0.10 0.45
SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE memo @@> 'cable OR relay' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: search_join
-- expect: scan 0.11 0.40
SELECT d.tier, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id WHERE e.memo @@> 'plug' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: scope_path_filter_in
-- expect: either 1.7 6.7
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id IN (SELECT d.id FROM dims d WHERE d.settings ->> 'tier' = 'tier_b') AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: scope_path_filter_join
-- expect: either 1.7 6.7
SELECT e.kind, count(*) AS n, sum(e.amount) AS total FROM events e JOIN dims d ON d.id = e.parent_id
WHERE d.settings ->> 'tier' = 'tier_c' AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: scope_expression_filter
-- expect: either 1.9 7.8
SELECT kind, count(*) AS n FROM events WHERE parent_id IN (SELECT d.id FROM dims d WHERE d.id % 3 = 1) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: skew_heaviest_parents
-- expect: scan 0.11 0.34
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE skew_id IN (SELECT id FROM dims WHERE id <= 5) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: skew_rarest_parents
-- expect: scan 0.11 0.34
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE skew_id IN (SELECT id FROM dims WHERE id > 55) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: skew_join_rows_read
-- expect: either 0.11 0.43
SELECT p.id, p.name, count(*) FILTER (WHERE e.ts >= date '2024-06-01') AS cur, count(*) FILTER (WHERE e.ts < date '2024-06-01') AS prev, avg(e.amount) AS amount
FROM events e JOIN parents p ON p.id = e.skew_id WHERE e.deleted_at IS NULL AND e.skew_id IN (1, 2, 3, 4, 5, 6) AND e.ts >= date '2024-05-01' AND e.ts < date '2024-07-01'
GROUP BY p.id, p.name ORDER BY cur DESC, p.id LIMIT 20;

-- name: dirty_pages_set
-- expect: scan 0.38 1.5
SELECT kind, count(*) AS n, sum(amount) AS total FROM events_dirty WHERE set_id = 3 AND ts >= date '2024-01-01' AND ts < date '2025-01-01' GROUP BY 1 ORDER BY 1;

-- name: flat_list
-- expect: scan 0.09 0.34
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (SELECT id FROM dims WHERE id <= 5) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: any_array_subselect
-- expect: scan 0.043 0.17
SELECT kind, count(*) AS n FROM events WHERE parent_id = ANY (ARRAY (SELECT id FROM dims WHERE active AND id % 5 = 0)) AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: in_constants
-- expect: scan 0.09 0.37
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (1, 2, 3, 4, 5, 6, 7, 8) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: point_day_count
-- expect: scan 0.34 1.4
SELECT count(*) AS n, sum(amount) AS total FROM events WHERE ts = date '2025-03-01';

-- name: point_days_three
-- expect: scan 0.12 0.50
SELECT count(*) AS n, sum(amount) AS total FROM events WHERE ts IN (date '2024-03-01', date '2024-06-01', date '2025-03-01');

-- name: point_days_ten
-- expect: scan 0.051 0.20
SELECT count(*) AS n, sum(amount) AS total FROM events WHERE ts IN (date '2024-01-05', date '2024-02-05', date '2024-03-05', date '2024-04-05', date '2024-05-05', date '2024-06-05', date '2024-07-05', date '2024-08-05', date '2024-09-05', date '2024-10-05');

-- name: range_only_by_month
-- expect: either 0.21 0.70
SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: one_day
-- expect: either 0.85 3.5
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE ts = date '2025-03-01' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: deleted_rows
-- expect: scan 0.076 0.30
SELECT kind, count(*) AS n FROM events WHERE deleted_at IS NOT NULL GROUP BY 1 ORDER BY 1;

-- name: text_not_equal
-- expect: scan 0.10 0.32
SELECT parent_id, count(*) AS n FROM events WHERE kind <> 'cat_a' AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- an aggregate over an expression of a column of the table and a column of a lookup: PostgreSQL's numeric product per row, the scan's per distinct pair of values
-- name: dimexpr_dim_c
-- expect: scan 0.16 0.62
SELECT l.status, sum(l.amount * o.category_id) AS total FROM facts_b l JOIN dim_c o ON o.id = l.dim_a_id GROUP BY 1 ORDER BY 1;

-- name: dimexpr_two
-- expect: scan 0.16 0.65
SELECT o.category_id, sum(l.amount * k.set_id) AS total FROM facts_b l JOIN dim_b k ON k.id = l.foreign_id JOIN dim_c o ON o.id = l.dim_a_id GROUP BY 1 ORDER BY 1;

-- name: dimsum_two
-- expect: scan 0.18 0.70
SELECT o.category_id, k.set_id, sum(l.amount) AS total FROM facts_b l JOIN dim_b k ON k.id = l.foreign_id JOIN dim_c o ON o.id = l.dim_a_id GROUP BY 1, 2 ORDER BY 1, 2;

-- the same expression over the rows of a few sets: the b-tree on dim_a_id reads them
-- name: dimexpr_few_sets
-- expect: normal 1.0 4.1
SELECT o.category_id, sum(l.amount * k.set_id) AS total FROM facts_b l JOIN dim_b k ON k.id = l.foreign_id JOIN dim_c o ON o.id = l.dim_a_id WHERE l.dim_a_id IN (3, 5, 8) GROUP BY 1 ORDER BY 1;

-- a rate of a few values: the scan evaluates the product for each distinct pair, PostgreSQL for each row
-- name: dimrate_one
-- expect: scan 0.080 0.32
SELECT l.status, sum(l.rate * k.set_id) AS total FROM facts_b l JOIN dim_b k ON k.id = l.foreign_id GROUP BY 1 ORDER BY 1;

-- name: dimrate_two
-- expect: scan 0.16 0.65
SELECT o.category_id, sum(l.rate * k.set_id) AS total FROM facts_b l JOIN dim_b k ON k.id = l.foreign_id JOIN dim_c o ON o.id = l.dim_a_id GROUP BY 1 ORDER BY 1;


-- the rows of a lookup that no row of the table points to are groups too: RIGHT and FULL JOIN of the table to a lookup (the scan adds the lookup rows that matched nothing)
-- name: dimright_dim_c
-- expect: either 0.19 0.76
SELECT c.category_id, count(f.id) AS n, sum(f.rate) AS total FROM facts_b f RIGHT JOIN dim_c c ON c.id = f.dim_a_id GROUP BY 1 ORDER BY 1;

-- name: dimfull_dim_b
-- expect: either 0.15 0.59
SELECT b.set_id, count(f.id) AS n, sum(f.rate) AS total FROM facts_b f FULL JOIN dim_b b ON b.id = f.foreign_id GROUP BY 1 ORDER BY 1;

-- name: dimright_filtered
-- expect: either 0.29 1.2
SELECT c.category_id, count(f.id) AS n FROM facts_b f RIGHT JOIN dim_c c ON c.id = f.dim_a_id AND f.status = 'paid' GROUP BY 1 ORDER BY 1;

-- a table of four times the rows (60 entries for each parent) joined by the parent column, to the rows of 200 and 2,000 parents and to all of them: the scan reads the entries of the keys (or the table whole), the normal plan probes the btree for each key or reads the table
-- the entries of the parents (a join that multiplies the rows)
-- name: linkjoin_200_parents
-- runs: 9
-- expect: scan 0.047 0.19
SELECT l.label, count(*) AS n, sum(l.weight) AS w FROM facts_b f JOIN links_b l ON l.parent_id = f.dim_a_id AND l.status = 0 WHERE f.dim_a_id IN (1,11,21,31,41,51,61,71,81,91,101,111,121,131,141,151,161,171,181,191,201,211,221,231,241,251,261,271,281,291,301,311,321,331,341,351,361,371,381,391,401,411,421,431,441,451,461,471,481,491,501,511,521,531,541,551,561,571,581,591,601,611,621,631,641,651,661,671,681,691,701,711,721,731,741,751,761,771,781,791,801,811,821,831,841,851,861,871,881,891,901,911,921,931,941,951,961,971,981,991,1001,1011,1021,1031,1041,1051,1061,1071,1081,1091,1101,1111,1121,1131,1141,1151,1161,1171,1181,1191,1201,1211,1221,1231,1241,1251,1261,1271,1281,1291,1301,1311,1321,1331,1341,1351,1361,1371,1381,1391,1401,1411,1421,1431,1441,1451,1461,1471,1481,1491,1501,1511,1521,1531,1541,1551,1561,1571,1581,1591,1601,1611,1621,1631,1641,1651,1661,1671,1681,1691,1701,1711,1721,1731,1741,1751,1761,1771,1781,1791,1801,1811,1821,1831,1841,1851,1861,1871,1881,1891,1901,1911,1921,1931,1941,1951,1961,1971,1981,1991) GROUP BY 1 ORDER BY 1;

-- the rows that have an entry (EXISTS)
-- name: linksemi_200_parents
-- runs: 9
-- expect: scan 0.085 0.34
SELECT f.status, count(*) AS n FROM facts_b f WHERE EXISTS (SELECT 1 FROM links_b l WHERE l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1) AND f.dim_a_id IN (1,11,21,31,41,51,61,71,81,91,101,111,121,131,141,151,161,171,181,191,201,211,221,231,241,251,261,271,281,291,301,311,321,331,341,351,361,371,381,391,401,411,421,431,441,451,461,471,481,491,501,511,521,531,541,551,561,571,581,591,601,611,621,631,641,651,661,671,681,691,701,711,721,731,741,751,761,771,781,791,801,811,821,831,841,851,861,871,881,891,901,911,921,931,941,951,961,971,981,991,1001,1011,1021,1031,1041,1051,1061,1071,1081,1091,1101,1111,1121,1131,1141,1151,1161,1171,1181,1191,1201,1211,1221,1231,1241,1251,1261,1271,1281,1291,1301,1311,1321,1331,1341,1351,1361,1371,1381,1391,1401,1411,1421,1431,1441,1451,1461,1471,1481,1491,1501,1511,1521,1531,1541,1551,1561,1571,1581,1591,1601,1611,1621,1631,1641,1651,1661,1671,1681,1691,1701,1711,1721,1731,1741,1751,1761,1771,1781,1791,1801,1811,1821,1831,1841,1851,1861,1871,1881,1891,1901,1911,1921,1931,1941,1951,1961,1971,1981,1991) GROUP BY 1 ORDER BY 1;

-- the rows that have no entry (NOT EXISTS)
-- name: linkanti_200_parents
-- runs: 9
-- expect: scan 0.045 0.18
SELECT f.status, count(*) AS n FROM facts_b f WHERE NOT EXISTS (SELECT 1 FROM links_b l WHERE l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1) AND f.dim_a_id IN (1,11,21,31,41,51,61,71,81,91,101,111,121,131,141,151,161,171,181,191,201,211,221,231,241,251,261,271,281,291,301,311,321,331,341,351,361,371,381,391,401,411,421,431,441,451,461,471,481,491,501,511,521,531,541,551,561,571,581,591,601,611,621,631,641,651,661,671,681,691,701,711,721,731,741,751,761,771,781,791,801,811,821,831,841,851,861,871,881,891,901,911,921,931,941,951,961,971,981,991,1001,1011,1021,1031,1041,1051,1061,1071,1081,1091,1101,1111,1121,1131,1141,1151,1161,1171,1181,1191,1201,1211,1221,1231,1241,1251,1261,1271,1281,1291,1301,1311,1321,1331,1341,1351,1361,1371,1381,1391,1401,1411,1421,1431,1441,1451,1461,1471,1481,1491,1501,1511,1521,1531,1541,1551,1561,1571,1581,1591,1601,1611,1621,1631,1641,1651,1661,1671,1681,1691,1701,1711,1721,1731,1741,1751,1761,1771,1781,1791,1801,1811,1821,1831,1841,1851,1861,1871,1881,1891,1901,1911,1921,1931,1941,1951,1961,1971,1981,1991) GROUP BY 1 ORDER BY 1;

-- the rows with their entries, kept without one (LEFT JOIN)
-- name: linkleft_200_parents
-- runs: 9
-- expect: scan 0.050 0.20
SELECT f.status, count(l.id) AS n FROM facts_b f LEFT JOIN links_b l ON l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1 WHERE f.dim_a_id IN (1,11,21,31,41,51,61,71,81,91,101,111,121,131,141,151,161,171,181,191,201,211,221,231,241,251,261,271,281,291,301,311,321,331,341,351,361,371,381,391,401,411,421,431,441,451,461,471,481,491,501,511,521,531,541,551,561,571,581,591,601,611,621,631,641,651,661,671,681,691,701,711,721,731,741,751,761,771,781,791,801,811,821,831,841,851,861,871,881,891,901,911,921,931,941,951,961,971,981,991,1001,1011,1021,1031,1041,1051,1061,1071,1081,1091,1101,1111,1121,1131,1141,1151,1161,1171,1181,1191,1201,1211,1221,1231,1241,1251,1261,1271,1281,1291,1301,1311,1321,1331,1341,1351,1361,1371,1381,1391,1401,1411,1421,1431,1441,1451,1461,1471,1481,1491,1501,1511,1521,1531,1541,1551,1561,1571,1581,1591,1601,1611,1621,1631,1641,1651,1661,1671,1681,1691,1701,1711,1721,1731,1741,1751,1761,1771,1781,1791,1801,1811,1821,1831,1841,1851,1861,1871,1881,1891,1901,1911,1921,1931,1941,1951,1961,1971,1981,1991) GROUP BY 1 ORDER BY 1;

-- the entries of the parents (a join that multiplies the rows)
-- name: linkjoin_2000_parents
-- runs: 9
-- expect: scan 0.056 0.22
SELECT l.label, count(*) AS n, sum(l.weight) AS w FROM facts_b f JOIN links_b l ON l.parent_id = f.dim_a_id AND l.status = 0 WHERE f.dim_a_id IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,618,619,620,621,622,623,624,625,626,627,628,629,630,631,632,633,634,635,636,637,638,639,640,641,642,643,644,645,646,647,648,649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,777,778,779,780,781,782,783,784,785,786,787,788,789,790,791,792,793,794,795,796,797,798,799,800,801,802,803,804,805,806,807,808,809,810,811,812,813,814,815,816,817,818,819,820,821,822,823,824,825,826,827,828,829,830,831,832,833,834,835,836,837,838,839,840,841,842,843,844,845,846,847,848,849,850,851,852,853,854,855,856,857,858,859,860,861,862,863,864,865,866,867,868,869,870,871,872,873,874,875,876,877,878,879,880,881,882,883,884,885,886,887,888,889,890,891,892,893,894,895,896,897,898,899,900,901,902,903,904,905,906,907,908,909,910,911,912,913,914,915,916,917,918,919,920,921,922,923,924,925,926,927,928,929,930,931,932,933,934,935,936,937,938,939,940,941,942,943,944,945,946,947,948,949,950,951,952,953,954,955,956,957,958,959,960,961,962,963,964,965,966,967,968,969,970,971,972,973,974,975,976,977,978,979,980,981,982,983,984,985,986,987,988,989,990,991,992,993,994,995,996,997,998,999,1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,1015,1016,1017,1018,1019,1020,1021,1022,1023,1024,1025,1026,1027,1028,1029,1030,1031,1032,1033,1034,1035,1036,1037,1038,1039,1040,1041,1042,1043,1044,1045,1046,1047,1048,1049,1050,1051,1052,1053,1054,1055,1056,1057,1058,1059,1060,1061,1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,1073,1074,1075,1076,1077,1078,1079,1080,1081,1082,1083,1084,1085,1086,1087,1088,1089,1090,1091,1092,1093,1094,1095,1096,1097,1098,1099,1100,1101,1102,1103,1104,1105,1106,1107,1108,1109,1110,1111,1112,1113,1114,1115,1116,1117,1118,1119,1120,1121,1122,1123,1124,1125,1126,1127,1128,1129,1130,1131,1132,1133,1134,1135,1136,1137,1138,1139,1140,1141,1142,1143,1144,1145,1146,1147,1148,1149,1150,1151,1152,1153,1154,1155,1156,1157,1158,1159,1160,1161,1162,1163,1164,1165,1166,1167,1168,1169,1170,1171,1172,1173,1174,1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185,1186,1187,1188,1189,1190,1191,1192,1193,1194,1195,1196,1197,1198,1199,1200,1201,1202,1203,1204,1205,1206,1207,1208,1209,1210,1211,1212,1213,1214,1215,1216,1217,1218,1219,1220,1221,1222,1223,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,1234,1235,1236,1237,1238,1239,1240,1241,1242,1243,1244,1245,1246,1247,1248,1249,1250,1251,1252,1253,1254,1255,1256,1257,1258,1259,1260,1261,1262,1263,1264,1265,1266,1267,1268,1269,1270,1271,1272,1273,1274,1275,1276,1277,1278,1279,1280,1281,1282,1283,1284,1285,1286,1287,1288,1289,1290,1291,1292,1293,1294,1295,1296,1297,1298,1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,1313,1314,1315,1316,1317,1318,1319,1320,1321,1322,1323,1324,1325,1326,1327,1328,1329,1330,1331,1332,1333,1334,1335,1336,1337,1338,1339,1340,1341,1342,1343,1344,1345,1346,1347,1348,1349,1350,1351,1352,1353,1354,1355,1356,1357,1358,1359,1360,1361,1362,1363,1364,1365,1366,1367,1368,1369,1370,1371,1372,1373,1374,1375,1376,1377,1378,1379,1380,1381,1382,1383,1384,1385,1386,1387,1388,1389,1390,1391,1392,1393,1394,1395,1396,1397,1398,1399,1400,1401,1402,1403,1404,1405,1406,1407,1408,1409,1410,1411,1412,1413,1414,1415,1416,1417,1418,1419,1420,1421,1422,1423,1424,1425,1426,1427,1428,1429,1430,1431,1432,1433,1434,1435,1436,1437,1438,1439,1440,1441,1442,1443,1444,1445,1446,1447,1448,1449,1450,1451,1452,1453,1454,1455,1456,1457,1458,1459,1460,1461,1462,1463,1464,1465,1466,1467,1468,1469,1470,1471,1472,1473,1474,1475,1476,1477,1478,1479,1480,1481,1482,1483,1484,1485,1486,1487,1488,1489,1490,1491,1492,1493,1494,1495,1496,1497,1498,1499,1500,1501,1502,1503,1504,1505,1506,1507,1508,1509,1510,1511,1512,1513,1514,1515,1516,1517,1518,1519,1520,1521,1522,1523,1524,1525,1526,1527,1528,1529,1530,1531,1532,1533,1534,1535,1536,1537,1538,1539,1540,1541,1542,1543,1544,1545,1546,1547,1548,1549,1550,1551,1552,1553,1554,1555,1556,1557,1558,1559,1560,1561,1562,1563,1564,1565,1566,1567,1568,1569,1570,1571,1572,1573,1574,1575,1576,1577,1578,1579,1580,1581,1582,1583,1584,1585,1586,1587,1588,1589,1590,1591,1592,1593,1594,1595,1596,1597,1598,1599,1600,1601,1602,1603,1604,1605,1606,1607,1608,1609,1610,1611,1612,1613,1614,1615,1616,1617,1618,1619,1620,1621,1622,1623,1624,1625,1626,1627,1628,1629,1630,1631,1632,1633,1634,1635,1636,1637,1638,1639,1640,1641,1642,1643,1644,1645,1646,1647,1648,1649,1650,1651,1652,1653,1654,1655,1656,1657,1658,1659,1660,1661,1662,1663,1664,1665,1666,1667,1668,1669,1670,1671,1672,1673,1674,1675,1676,1677,1678,1679,1680,1681,1682,1683,1684,1685,1686,1687,1688,1689,1690,1691,1692,1693,1694,1695,1696,1697,1698,1699,1700,1701,1702,1703,1704,1705,1706,1707,1708,1709,1710,1711,1712,1713,1714,1715,1716,1717,1718,1719,1720,1721,1722,1723,1724,1725,1726,1727,1728,1729,1730,1731,1732,1733,1734,1735,1736,1737,1738,1739,1740,1741,1742,1743,1744,1745,1746,1747,1748,1749,1750,1751,1752,1753,1754,1755,1756,1757,1758,1759,1760,1761,1762,1763,1764,1765,1766,1767,1768,1769,1770,1771,1772,1773,1774,1775,1776,1777,1778,1779,1780,1781,1782,1783,1784,1785,1786,1787,1788,1789,1790,1791,1792,1793,1794,1795,1796,1797,1798,1799,1800,1801,1802,1803,1804,1805,1806,1807,1808,1809,1810,1811,1812,1813,1814,1815,1816,1817,1818,1819,1820,1821,1822,1823,1824,1825,1826,1827,1828,1829,1830,1831,1832,1833,1834,1835,1836,1837,1838,1839,1840,1841,1842,1843,1844,1845,1846,1847,1848,1849,1850,1851,1852,1853,1854,1855,1856,1857,1858,1859,1860,1861,1862,1863,1864,1865,1866,1867,1868,1869,1870,1871,1872,1873,1874,1875,1876,1877,1878,1879,1880,1881,1882,1883,1884,1885,1886,1887,1888,1889,1890,1891,1892,1893,1894,1895,1896,1897,1898,1899,1900,1901,1902,1903,1904,1905,1906,1907,1908,1909,1910,1911,1912,1913,1914,1915,1916,1917,1918,1919,1920,1921,1922,1923,1924,1925,1926,1927,1928,1929,1930,1931,1932,1933,1934,1935,1936,1937,1938,1939,1940,1941,1942,1943,1944,1945,1946,1947,1948,1949,1950,1951,1952,1953,1954,1955,1956,1957,1958,1959,1960,1961,1962,1963,1964,1965,1966,1967,1968,1969,1970,1971,1972,1973,1974,1975,1976,1977,1978,1979,1980,1981,1982,1983,1984,1985,1986,1987,1988,1989,1990,1991,1992,1993,1994,1995,1996,1997,1998,1999,2000) GROUP BY 1 ORDER BY 1;

-- the rows that have an entry (EXISTS)
-- name: linksemi_2000_parents
-- runs: 9
-- expect: scan 0.21 0.83
SELECT f.status, count(*) AS n FROM facts_b f WHERE EXISTS (SELECT 1 FROM links_b l WHERE l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1) AND f.dim_a_id IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,618,619,620,621,622,623,624,625,626,627,628,629,630,631,632,633,634,635,636,637,638,639,640,641,642,643,644,645,646,647,648,649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,777,778,779,780,781,782,783,784,785,786,787,788,789,790,791,792,793,794,795,796,797,798,799,800,801,802,803,804,805,806,807,808,809,810,811,812,813,814,815,816,817,818,819,820,821,822,823,824,825,826,827,828,829,830,831,832,833,834,835,836,837,838,839,840,841,842,843,844,845,846,847,848,849,850,851,852,853,854,855,856,857,858,859,860,861,862,863,864,865,866,867,868,869,870,871,872,873,874,875,876,877,878,879,880,881,882,883,884,885,886,887,888,889,890,891,892,893,894,895,896,897,898,899,900,901,902,903,904,905,906,907,908,909,910,911,912,913,914,915,916,917,918,919,920,921,922,923,924,925,926,927,928,929,930,931,932,933,934,935,936,937,938,939,940,941,942,943,944,945,946,947,948,949,950,951,952,953,954,955,956,957,958,959,960,961,962,963,964,965,966,967,968,969,970,971,972,973,974,975,976,977,978,979,980,981,982,983,984,985,986,987,988,989,990,991,992,993,994,995,996,997,998,999,1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,1015,1016,1017,1018,1019,1020,1021,1022,1023,1024,1025,1026,1027,1028,1029,1030,1031,1032,1033,1034,1035,1036,1037,1038,1039,1040,1041,1042,1043,1044,1045,1046,1047,1048,1049,1050,1051,1052,1053,1054,1055,1056,1057,1058,1059,1060,1061,1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,1073,1074,1075,1076,1077,1078,1079,1080,1081,1082,1083,1084,1085,1086,1087,1088,1089,1090,1091,1092,1093,1094,1095,1096,1097,1098,1099,1100,1101,1102,1103,1104,1105,1106,1107,1108,1109,1110,1111,1112,1113,1114,1115,1116,1117,1118,1119,1120,1121,1122,1123,1124,1125,1126,1127,1128,1129,1130,1131,1132,1133,1134,1135,1136,1137,1138,1139,1140,1141,1142,1143,1144,1145,1146,1147,1148,1149,1150,1151,1152,1153,1154,1155,1156,1157,1158,1159,1160,1161,1162,1163,1164,1165,1166,1167,1168,1169,1170,1171,1172,1173,1174,1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185,1186,1187,1188,1189,1190,1191,1192,1193,1194,1195,1196,1197,1198,1199,1200,1201,1202,1203,1204,1205,1206,1207,1208,1209,1210,1211,1212,1213,1214,1215,1216,1217,1218,1219,1220,1221,1222,1223,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,1234,1235,1236,1237,1238,1239,1240,1241,1242,1243,1244,1245,1246,1247,1248,1249,1250,1251,1252,1253,1254,1255,1256,1257,1258,1259,1260,1261,1262,1263,1264,1265,1266,1267,1268,1269,1270,1271,1272,1273,1274,1275,1276,1277,1278,1279,1280,1281,1282,1283,1284,1285,1286,1287,1288,1289,1290,1291,1292,1293,1294,1295,1296,1297,1298,1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,1313,1314,1315,1316,1317,1318,1319,1320,1321,1322,1323,1324,1325,1326,1327,1328,1329,1330,1331,1332,1333,1334,1335,1336,1337,1338,1339,1340,1341,1342,1343,1344,1345,1346,1347,1348,1349,1350,1351,1352,1353,1354,1355,1356,1357,1358,1359,1360,1361,1362,1363,1364,1365,1366,1367,1368,1369,1370,1371,1372,1373,1374,1375,1376,1377,1378,1379,1380,1381,1382,1383,1384,1385,1386,1387,1388,1389,1390,1391,1392,1393,1394,1395,1396,1397,1398,1399,1400,1401,1402,1403,1404,1405,1406,1407,1408,1409,1410,1411,1412,1413,1414,1415,1416,1417,1418,1419,1420,1421,1422,1423,1424,1425,1426,1427,1428,1429,1430,1431,1432,1433,1434,1435,1436,1437,1438,1439,1440,1441,1442,1443,1444,1445,1446,1447,1448,1449,1450,1451,1452,1453,1454,1455,1456,1457,1458,1459,1460,1461,1462,1463,1464,1465,1466,1467,1468,1469,1470,1471,1472,1473,1474,1475,1476,1477,1478,1479,1480,1481,1482,1483,1484,1485,1486,1487,1488,1489,1490,1491,1492,1493,1494,1495,1496,1497,1498,1499,1500,1501,1502,1503,1504,1505,1506,1507,1508,1509,1510,1511,1512,1513,1514,1515,1516,1517,1518,1519,1520,1521,1522,1523,1524,1525,1526,1527,1528,1529,1530,1531,1532,1533,1534,1535,1536,1537,1538,1539,1540,1541,1542,1543,1544,1545,1546,1547,1548,1549,1550,1551,1552,1553,1554,1555,1556,1557,1558,1559,1560,1561,1562,1563,1564,1565,1566,1567,1568,1569,1570,1571,1572,1573,1574,1575,1576,1577,1578,1579,1580,1581,1582,1583,1584,1585,1586,1587,1588,1589,1590,1591,1592,1593,1594,1595,1596,1597,1598,1599,1600,1601,1602,1603,1604,1605,1606,1607,1608,1609,1610,1611,1612,1613,1614,1615,1616,1617,1618,1619,1620,1621,1622,1623,1624,1625,1626,1627,1628,1629,1630,1631,1632,1633,1634,1635,1636,1637,1638,1639,1640,1641,1642,1643,1644,1645,1646,1647,1648,1649,1650,1651,1652,1653,1654,1655,1656,1657,1658,1659,1660,1661,1662,1663,1664,1665,1666,1667,1668,1669,1670,1671,1672,1673,1674,1675,1676,1677,1678,1679,1680,1681,1682,1683,1684,1685,1686,1687,1688,1689,1690,1691,1692,1693,1694,1695,1696,1697,1698,1699,1700,1701,1702,1703,1704,1705,1706,1707,1708,1709,1710,1711,1712,1713,1714,1715,1716,1717,1718,1719,1720,1721,1722,1723,1724,1725,1726,1727,1728,1729,1730,1731,1732,1733,1734,1735,1736,1737,1738,1739,1740,1741,1742,1743,1744,1745,1746,1747,1748,1749,1750,1751,1752,1753,1754,1755,1756,1757,1758,1759,1760,1761,1762,1763,1764,1765,1766,1767,1768,1769,1770,1771,1772,1773,1774,1775,1776,1777,1778,1779,1780,1781,1782,1783,1784,1785,1786,1787,1788,1789,1790,1791,1792,1793,1794,1795,1796,1797,1798,1799,1800,1801,1802,1803,1804,1805,1806,1807,1808,1809,1810,1811,1812,1813,1814,1815,1816,1817,1818,1819,1820,1821,1822,1823,1824,1825,1826,1827,1828,1829,1830,1831,1832,1833,1834,1835,1836,1837,1838,1839,1840,1841,1842,1843,1844,1845,1846,1847,1848,1849,1850,1851,1852,1853,1854,1855,1856,1857,1858,1859,1860,1861,1862,1863,1864,1865,1866,1867,1868,1869,1870,1871,1872,1873,1874,1875,1876,1877,1878,1879,1880,1881,1882,1883,1884,1885,1886,1887,1888,1889,1890,1891,1892,1893,1894,1895,1896,1897,1898,1899,1900,1901,1902,1903,1904,1905,1906,1907,1908,1909,1910,1911,1912,1913,1914,1915,1916,1917,1918,1919,1920,1921,1922,1923,1924,1925,1926,1927,1928,1929,1930,1931,1932,1933,1934,1935,1936,1937,1938,1939,1940,1941,1942,1943,1944,1945,1946,1947,1948,1949,1950,1951,1952,1953,1954,1955,1956,1957,1958,1959,1960,1961,1962,1963,1964,1965,1966,1967,1968,1969,1970,1971,1972,1973,1974,1975,1976,1977,1978,1979,1980,1981,1982,1983,1984,1985,1986,1987,1988,1989,1990,1991,1992,1993,1994,1995,1996,1997,1998,1999,2000) GROUP BY 1 ORDER BY 1;

-- the rows that have no entry (NOT EXISTS)
-- name: linkanti_2000_parents
-- runs: 9
-- expect: scan 0.16 0.62
SELECT f.status, count(*) AS n FROM facts_b f WHERE NOT EXISTS (SELECT 1 FROM links_b l WHERE l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1) AND f.dim_a_id IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,618,619,620,621,622,623,624,625,626,627,628,629,630,631,632,633,634,635,636,637,638,639,640,641,642,643,644,645,646,647,648,649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,777,778,779,780,781,782,783,784,785,786,787,788,789,790,791,792,793,794,795,796,797,798,799,800,801,802,803,804,805,806,807,808,809,810,811,812,813,814,815,816,817,818,819,820,821,822,823,824,825,826,827,828,829,830,831,832,833,834,835,836,837,838,839,840,841,842,843,844,845,846,847,848,849,850,851,852,853,854,855,856,857,858,859,860,861,862,863,864,865,866,867,868,869,870,871,872,873,874,875,876,877,878,879,880,881,882,883,884,885,886,887,888,889,890,891,892,893,894,895,896,897,898,899,900,901,902,903,904,905,906,907,908,909,910,911,912,913,914,915,916,917,918,919,920,921,922,923,924,925,926,927,928,929,930,931,932,933,934,935,936,937,938,939,940,941,942,943,944,945,946,947,948,949,950,951,952,953,954,955,956,957,958,959,960,961,962,963,964,965,966,967,968,969,970,971,972,973,974,975,976,977,978,979,980,981,982,983,984,985,986,987,988,989,990,991,992,993,994,995,996,997,998,999,1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,1015,1016,1017,1018,1019,1020,1021,1022,1023,1024,1025,1026,1027,1028,1029,1030,1031,1032,1033,1034,1035,1036,1037,1038,1039,1040,1041,1042,1043,1044,1045,1046,1047,1048,1049,1050,1051,1052,1053,1054,1055,1056,1057,1058,1059,1060,1061,1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,1073,1074,1075,1076,1077,1078,1079,1080,1081,1082,1083,1084,1085,1086,1087,1088,1089,1090,1091,1092,1093,1094,1095,1096,1097,1098,1099,1100,1101,1102,1103,1104,1105,1106,1107,1108,1109,1110,1111,1112,1113,1114,1115,1116,1117,1118,1119,1120,1121,1122,1123,1124,1125,1126,1127,1128,1129,1130,1131,1132,1133,1134,1135,1136,1137,1138,1139,1140,1141,1142,1143,1144,1145,1146,1147,1148,1149,1150,1151,1152,1153,1154,1155,1156,1157,1158,1159,1160,1161,1162,1163,1164,1165,1166,1167,1168,1169,1170,1171,1172,1173,1174,1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185,1186,1187,1188,1189,1190,1191,1192,1193,1194,1195,1196,1197,1198,1199,1200,1201,1202,1203,1204,1205,1206,1207,1208,1209,1210,1211,1212,1213,1214,1215,1216,1217,1218,1219,1220,1221,1222,1223,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,1234,1235,1236,1237,1238,1239,1240,1241,1242,1243,1244,1245,1246,1247,1248,1249,1250,1251,1252,1253,1254,1255,1256,1257,1258,1259,1260,1261,1262,1263,1264,1265,1266,1267,1268,1269,1270,1271,1272,1273,1274,1275,1276,1277,1278,1279,1280,1281,1282,1283,1284,1285,1286,1287,1288,1289,1290,1291,1292,1293,1294,1295,1296,1297,1298,1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,1313,1314,1315,1316,1317,1318,1319,1320,1321,1322,1323,1324,1325,1326,1327,1328,1329,1330,1331,1332,1333,1334,1335,1336,1337,1338,1339,1340,1341,1342,1343,1344,1345,1346,1347,1348,1349,1350,1351,1352,1353,1354,1355,1356,1357,1358,1359,1360,1361,1362,1363,1364,1365,1366,1367,1368,1369,1370,1371,1372,1373,1374,1375,1376,1377,1378,1379,1380,1381,1382,1383,1384,1385,1386,1387,1388,1389,1390,1391,1392,1393,1394,1395,1396,1397,1398,1399,1400,1401,1402,1403,1404,1405,1406,1407,1408,1409,1410,1411,1412,1413,1414,1415,1416,1417,1418,1419,1420,1421,1422,1423,1424,1425,1426,1427,1428,1429,1430,1431,1432,1433,1434,1435,1436,1437,1438,1439,1440,1441,1442,1443,1444,1445,1446,1447,1448,1449,1450,1451,1452,1453,1454,1455,1456,1457,1458,1459,1460,1461,1462,1463,1464,1465,1466,1467,1468,1469,1470,1471,1472,1473,1474,1475,1476,1477,1478,1479,1480,1481,1482,1483,1484,1485,1486,1487,1488,1489,1490,1491,1492,1493,1494,1495,1496,1497,1498,1499,1500,1501,1502,1503,1504,1505,1506,1507,1508,1509,1510,1511,1512,1513,1514,1515,1516,1517,1518,1519,1520,1521,1522,1523,1524,1525,1526,1527,1528,1529,1530,1531,1532,1533,1534,1535,1536,1537,1538,1539,1540,1541,1542,1543,1544,1545,1546,1547,1548,1549,1550,1551,1552,1553,1554,1555,1556,1557,1558,1559,1560,1561,1562,1563,1564,1565,1566,1567,1568,1569,1570,1571,1572,1573,1574,1575,1576,1577,1578,1579,1580,1581,1582,1583,1584,1585,1586,1587,1588,1589,1590,1591,1592,1593,1594,1595,1596,1597,1598,1599,1600,1601,1602,1603,1604,1605,1606,1607,1608,1609,1610,1611,1612,1613,1614,1615,1616,1617,1618,1619,1620,1621,1622,1623,1624,1625,1626,1627,1628,1629,1630,1631,1632,1633,1634,1635,1636,1637,1638,1639,1640,1641,1642,1643,1644,1645,1646,1647,1648,1649,1650,1651,1652,1653,1654,1655,1656,1657,1658,1659,1660,1661,1662,1663,1664,1665,1666,1667,1668,1669,1670,1671,1672,1673,1674,1675,1676,1677,1678,1679,1680,1681,1682,1683,1684,1685,1686,1687,1688,1689,1690,1691,1692,1693,1694,1695,1696,1697,1698,1699,1700,1701,1702,1703,1704,1705,1706,1707,1708,1709,1710,1711,1712,1713,1714,1715,1716,1717,1718,1719,1720,1721,1722,1723,1724,1725,1726,1727,1728,1729,1730,1731,1732,1733,1734,1735,1736,1737,1738,1739,1740,1741,1742,1743,1744,1745,1746,1747,1748,1749,1750,1751,1752,1753,1754,1755,1756,1757,1758,1759,1760,1761,1762,1763,1764,1765,1766,1767,1768,1769,1770,1771,1772,1773,1774,1775,1776,1777,1778,1779,1780,1781,1782,1783,1784,1785,1786,1787,1788,1789,1790,1791,1792,1793,1794,1795,1796,1797,1798,1799,1800,1801,1802,1803,1804,1805,1806,1807,1808,1809,1810,1811,1812,1813,1814,1815,1816,1817,1818,1819,1820,1821,1822,1823,1824,1825,1826,1827,1828,1829,1830,1831,1832,1833,1834,1835,1836,1837,1838,1839,1840,1841,1842,1843,1844,1845,1846,1847,1848,1849,1850,1851,1852,1853,1854,1855,1856,1857,1858,1859,1860,1861,1862,1863,1864,1865,1866,1867,1868,1869,1870,1871,1872,1873,1874,1875,1876,1877,1878,1879,1880,1881,1882,1883,1884,1885,1886,1887,1888,1889,1890,1891,1892,1893,1894,1895,1896,1897,1898,1899,1900,1901,1902,1903,1904,1905,1906,1907,1908,1909,1910,1911,1912,1913,1914,1915,1916,1917,1918,1919,1920,1921,1922,1923,1924,1925,1926,1927,1928,1929,1930,1931,1932,1933,1934,1935,1936,1937,1938,1939,1940,1941,1942,1943,1944,1945,1946,1947,1948,1949,1950,1951,1952,1953,1954,1955,1956,1957,1958,1959,1960,1961,1962,1963,1964,1965,1966,1967,1968,1969,1970,1971,1972,1973,1974,1975,1976,1977,1978,1979,1980,1981,1982,1983,1984,1985,1986,1987,1988,1989,1990,1991,1992,1993,1994,1995,1996,1997,1998,1999,2000) GROUP BY 1 ORDER BY 1;

-- the rows with their entries, kept without one (LEFT JOIN)
-- name: linkleft_2000_parents
-- runs: 9
-- expect: scan 0.12 0.47
SELECT f.status, count(l.id) AS n FROM facts_b f LEFT JOIN links_b l ON l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1 WHERE f.dim_a_id IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,618,619,620,621,622,623,624,625,626,627,628,629,630,631,632,633,634,635,636,637,638,639,640,641,642,643,644,645,646,647,648,649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,777,778,779,780,781,782,783,784,785,786,787,788,789,790,791,792,793,794,795,796,797,798,799,800,801,802,803,804,805,806,807,808,809,810,811,812,813,814,815,816,817,818,819,820,821,822,823,824,825,826,827,828,829,830,831,832,833,834,835,836,837,838,839,840,841,842,843,844,845,846,847,848,849,850,851,852,853,854,855,856,857,858,859,860,861,862,863,864,865,866,867,868,869,870,871,872,873,874,875,876,877,878,879,880,881,882,883,884,885,886,887,888,889,890,891,892,893,894,895,896,897,898,899,900,901,902,903,904,905,906,907,908,909,910,911,912,913,914,915,916,917,918,919,920,921,922,923,924,925,926,927,928,929,930,931,932,933,934,935,936,937,938,939,940,941,942,943,944,945,946,947,948,949,950,951,952,953,954,955,956,957,958,959,960,961,962,963,964,965,966,967,968,969,970,971,972,973,974,975,976,977,978,979,980,981,982,983,984,985,986,987,988,989,990,991,992,993,994,995,996,997,998,999,1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,1015,1016,1017,1018,1019,1020,1021,1022,1023,1024,1025,1026,1027,1028,1029,1030,1031,1032,1033,1034,1035,1036,1037,1038,1039,1040,1041,1042,1043,1044,1045,1046,1047,1048,1049,1050,1051,1052,1053,1054,1055,1056,1057,1058,1059,1060,1061,1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,1073,1074,1075,1076,1077,1078,1079,1080,1081,1082,1083,1084,1085,1086,1087,1088,1089,1090,1091,1092,1093,1094,1095,1096,1097,1098,1099,1100,1101,1102,1103,1104,1105,1106,1107,1108,1109,1110,1111,1112,1113,1114,1115,1116,1117,1118,1119,1120,1121,1122,1123,1124,1125,1126,1127,1128,1129,1130,1131,1132,1133,1134,1135,1136,1137,1138,1139,1140,1141,1142,1143,1144,1145,1146,1147,1148,1149,1150,1151,1152,1153,1154,1155,1156,1157,1158,1159,1160,1161,1162,1163,1164,1165,1166,1167,1168,1169,1170,1171,1172,1173,1174,1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185,1186,1187,1188,1189,1190,1191,1192,1193,1194,1195,1196,1197,1198,1199,1200,1201,1202,1203,1204,1205,1206,1207,1208,1209,1210,1211,1212,1213,1214,1215,1216,1217,1218,1219,1220,1221,1222,1223,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,1234,1235,1236,1237,1238,1239,1240,1241,1242,1243,1244,1245,1246,1247,1248,1249,1250,1251,1252,1253,1254,1255,1256,1257,1258,1259,1260,1261,1262,1263,1264,1265,1266,1267,1268,1269,1270,1271,1272,1273,1274,1275,1276,1277,1278,1279,1280,1281,1282,1283,1284,1285,1286,1287,1288,1289,1290,1291,1292,1293,1294,1295,1296,1297,1298,1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,1313,1314,1315,1316,1317,1318,1319,1320,1321,1322,1323,1324,1325,1326,1327,1328,1329,1330,1331,1332,1333,1334,1335,1336,1337,1338,1339,1340,1341,1342,1343,1344,1345,1346,1347,1348,1349,1350,1351,1352,1353,1354,1355,1356,1357,1358,1359,1360,1361,1362,1363,1364,1365,1366,1367,1368,1369,1370,1371,1372,1373,1374,1375,1376,1377,1378,1379,1380,1381,1382,1383,1384,1385,1386,1387,1388,1389,1390,1391,1392,1393,1394,1395,1396,1397,1398,1399,1400,1401,1402,1403,1404,1405,1406,1407,1408,1409,1410,1411,1412,1413,1414,1415,1416,1417,1418,1419,1420,1421,1422,1423,1424,1425,1426,1427,1428,1429,1430,1431,1432,1433,1434,1435,1436,1437,1438,1439,1440,1441,1442,1443,1444,1445,1446,1447,1448,1449,1450,1451,1452,1453,1454,1455,1456,1457,1458,1459,1460,1461,1462,1463,1464,1465,1466,1467,1468,1469,1470,1471,1472,1473,1474,1475,1476,1477,1478,1479,1480,1481,1482,1483,1484,1485,1486,1487,1488,1489,1490,1491,1492,1493,1494,1495,1496,1497,1498,1499,1500,1501,1502,1503,1504,1505,1506,1507,1508,1509,1510,1511,1512,1513,1514,1515,1516,1517,1518,1519,1520,1521,1522,1523,1524,1525,1526,1527,1528,1529,1530,1531,1532,1533,1534,1535,1536,1537,1538,1539,1540,1541,1542,1543,1544,1545,1546,1547,1548,1549,1550,1551,1552,1553,1554,1555,1556,1557,1558,1559,1560,1561,1562,1563,1564,1565,1566,1567,1568,1569,1570,1571,1572,1573,1574,1575,1576,1577,1578,1579,1580,1581,1582,1583,1584,1585,1586,1587,1588,1589,1590,1591,1592,1593,1594,1595,1596,1597,1598,1599,1600,1601,1602,1603,1604,1605,1606,1607,1608,1609,1610,1611,1612,1613,1614,1615,1616,1617,1618,1619,1620,1621,1622,1623,1624,1625,1626,1627,1628,1629,1630,1631,1632,1633,1634,1635,1636,1637,1638,1639,1640,1641,1642,1643,1644,1645,1646,1647,1648,1649,1650,1651,1652,1653,1654,1655,1656,1657,1658,1659,1660,1661,1662,1663,1664,1665,1666,1667,1668,1669,1670,1671,1672,1673,1674,1675,1676,1677,1678,1679,1680,1681,1682,1683,1684,1685,1686,1687,1688,1689,1690,1691,1692,1693,1694,1695,1696,1697,1698,1699,1700,1701,1702,1703,1704,1705,1706,1707,1708,1709,1710,1711,1712,1713,1714,1715,1716,1717,1718,1719,1720,1721,1722,1723,1724,1725,1726,1727,1728,1729,1730,1731,1732,1733,1734,1735,1736,1737,1738,1739,1740,1741,1742,1743,1744,1745,1746,1747,1748,1749,1750,1751,1752,1753,1754,1755,1756,1757,1758,1759,1760,1761,1762,1763,1764,1765,1766,1767,1768,1769,1770,1771,1772,1773,1774,1775,1776,1777,1778,1779,1780,1781,1782,1783,1784,1785,1786,1787,1788,1789,1790,1791,1792,1793,1794,1795,1796,1797,1798,1799,1800,1801,1802,1803,1804,1805,1806,1807,1808,1809,1810,1811,1812,1813,1814,1815,1816,1817,1818,1819,1820,1821,1822,1823,1824,1825,1826,1827,1828,1829,1830,1831,1832,1833,1834,1835,1836,1837,1838,1839,1840,1841,1842,1843,1844,1845,1846,1847,1848,1849,1850,1851,1852,1853,1854,1855,1856,1857,1858,1859,1860,1861,1862,1863,1864,1865,1866,1867,1868,1869,1870,1871,1872,1873,1874,1875,1876,1877,1878,1879,1880,1881,1882,1883,1884,1885,1886,1887,1888,1889,1890,1891,1892,1893,1894,1895,1896,1897,1898,1899,1900,1901,1902,1903,1904,1905,1906,1907,1908,1909,1910,1911,1912,1913,1914,1915,1916,1917,1918,1919,1920,1921,1922,1923,1924,1925,1926,1927,1928,1929,1930,1931,1932,1933,1934,1935,1936,1937,1938,1939,1940,1941,1942,1943,1944,1945,1946,1947,1948,1949,1950,1951,1952,1953,1954,1955,1956,1957,1958,1959,1960,1961,1962,1963,1964,1965,1966,1967,1968,1969,1970,1971,1972,1973,1974,1975,1976,1977,1978,1979,1980,1981,1982,1983,1984,1985,1986,1987,1988,1989,1990,1991,1992,1993,1994,1995,1996,1997,1998,1999,2000) GROUP BY 1 ORDER BY 1;

-- the entries of the parents (a join that multiplies the rows)
-- name: linkjoin_all_parents
-- runs: 9
-- expect: scan 0.056 0.23
SELECT l.label, count(*) AS n, sum(l.weight) AS w FROM facts_b f JOIN links_b l ON l.parent_id = f.dim_a_id AND l.status = 0  GROUP BY 1 ORDER BY 1;

-- the rows that have an entry (EXISTS)
-- name: linksemi_all_parents
-- runs: 9
-- expect: scan 0.37 1.5
SELECT f.status, count(*) AS n FROM facts_b f WHERE EXISTS (SELECT 1 FROM links_b l WHERE l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1) GROUP BY 1 ORDER BY 1;

-- the rows that have no entry (NOT EXISTS)
-- name: linkanti_all_parents
-- runs: 9
-- expect: scan 0.28 1.1
SELECT f.status, count(*) AS n FROM facts_b f WHERE NOT EXISTS (SELECT 1 FROM links_b l WHERE l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1) GROUP BY 1 ORDER BY 1;

-- the rows with their entries, kept without one (LEFT JOIN)
-- name: linkleft_all_parents
-- runs: 9
-- expect: scan 0.12 0.48
SELECT f.status, count(l.id) AS n FROM facts_b f LEFT JOIN links_b l ON l.parent_id = f.dim_a_id AND l.status = 0 AND l.weight = 1  GROUP BY 1 ORDER BY 1;

