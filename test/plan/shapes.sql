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
-- expect: normal 0.99 4.0
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

-- name: join_other_fact_filter
-- expect: scan 0.26 1.0
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
-- expect: scan 0.14 0.56
SELECT b.bucket, coalesce(e.n, 0) AS n, coalesce(e.total, 0) AS total FROM buckets b
LEFT JOIN (SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n, sum(amount) AS total FROM events WHERE deleted_at IS NULL AND parent_id = 7 GROUP BY 1) e ON e.bucket = b.bucket
ORDER BY 1;

-- name: spine_expression_key
-- expect: scan 0.10 0.35
SELECT b.bucket, count(e.id) AS n FROM buckets b LEFT JOIN events e ON date_trunc('month', e.ts)::date = b.bucket AND e.kind = 'cat_a' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: spine_span_kind
-- expect: scan 0.15 0.60
SELECT b.span, e.kind, count(*) AS n, sum(e.amount) AS total FROM buckets b JOIN events e ON e.ts >= b.bucket AND e.ts < b.bucket + interval '1 month'
WHERE e.deleted_at IS NULL GROUP BY 1, 2 ORDER BY 1, 2;

-- name: spine_series
-- expect: scan 0.16 0.62
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
-- expect: scan 0.16 0.65
SELECT e.kind, count(*) AS n FROM events e
WHERE EXISTS (SELECT 1 FROM facts f WHERE f.parent_id = e.parent_id AND f.role = 'type_a' AND f.val > 10) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: not_exists_in_table
-- expect: scan 0.11 0.42
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
-- expect: either 0.17 0.67
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
-- expect: scan 0.14 0.55
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

-- name: search_group
-- expect: scan 0.093 0.37
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE memo @@> 'cable wire' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: search_count
-- expect: normal -
SELECT count(*) AS n FROM events WHERE memo @@> 'cable';

-- name: search_one_parent
-- expect: either 0.20 0.81
SELECT kind, count(*) AS n FROM events WHERE memo @@> 'switch' AND parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: search_latest_rows
-- expect: scan 0.095 0.37
SELECT id, ts, amount FROM events WHERE memo @@> 'cable wire' AND deleted_at IS NULL ORDER BY ts DESC, id DESC LIMIT 8;

-- name: search_or_by_month
-- expect: scan 0.10 0.34
SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE memo @@> 'cable OR relay' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: search_join
-- expect: scan 0.11 0.40
SELECT d.tier, count(*) AS n FROM events e JOIN dims d ON d.id = e.parent_id WHERE e.memo @@> 'plug' AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: scope_path_filter_in
-- expect: either 3.8 16
SELECT kind, count(*) AS n, sum(amount) AS total FROM events
WHERE parent_id IN (SELECT d.id FROM dims d WHERE d.settings ->> 'tier' = 'tier_b') AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2024-03-08' GROUP BY 1 ORDER BY 1;

-- name: scope_path_filter_join
-- expect: either 3.8 16
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

-- name: flat_list
-- expect: scan 0.11 0.34
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (SELECT id FROM dims WHERE id <= 5) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: any_array_subselect
-- expect: scan 0.096 0.35
SELECT kind, count(*) AS n FROM events WHERE parent_id = ANY (ARRAY (SELECT id FROM dims WHERE active AND id % 5 = 0)) AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: in_constants
-- expect: scan 0.11 0.37
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (1, 2, 3, 4, 5, 6, 7, 8) AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: range_only_by_month
-- expect: either 0.21 0.70
SELECT date_trunc('month', ts)::date AS bucket, count(*) AS n FROM events WHERE ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;

-- name: one_day
-- expect: normal 0.85 3.5
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE ts = date '2025-03-01' AND deleted_at IS NULL GROUP BY 1 ORDER BY 1;

-- name: deleted_rows
-- expect: scan 0.076 0.30
SELECT kind, count(*) AS n FROM events WHERE deleted_at IS NOT NULL GROUP BY 1 ORDER BY 1;

-- name: text_not_equal
-- expect: scan 0.10 0.32
SELECT parent_id, count(*) AS n FROM events WHERE kind <> 'cat_a' AND deleted_at IS NULL AND ts >= date '2024-03-01' AND ts < date '2025-03-01' GROUP BY 1 ORDER BY 1;
