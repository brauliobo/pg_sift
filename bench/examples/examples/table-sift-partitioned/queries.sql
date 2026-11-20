-- query: events-by-kind-first-quarter
-- kind: partition_prune
-- join: inner
SELECT s.kind, count(*) AS n, sum(e.value) AS value FROM event e JOIN source s ON s.id = e.feed_id WHERE e.ts >= '2024-01-01' AND e.ts < '2024-04-01' GROUP BY s.kind;

-- query: events-by-month-and-region
-- kind: group_by_dim_attr
-- join: inner
SELECT date_trunc('month', e.ts) AS month, s.region, count(*) AS n FROM event e JOIN source s ON s.id = e.feed_id GROUP BY 1, 2;

-- query: one-month-in-region
-- kind: partition_prune
-- join: inner
SELECT s.kind, count(*) AS n FROM event e JOIN source s ON s.id = e.feed_id WHERE e.ts >= '2024-06-01' AND e.ts < '2024-07-01' AND s.region = 'r2' GROUP BY s.kind;

-- query: distinct-sources-by-kind
-- kind: count_distinct_across_join
-- join: inner
SELECT s.kind, count(DISTINCT e.feed_id) AS sources FROM event e JOIN source s ON s.id = e.feed_id GROUP BY s.kind;

-- query: errors-by-region
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT s.region, count(*) FILTER (WHERE e.status = 1) AS errors, count(*) FILTER (WHERE s.kind = 'w1') AS kind1, count(*) AS n FROM event e JOIN source s ON s.id = e.feed_id GROUP BY s.region;

-- query: top-sources-second-half
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT s.id, s.kind, sum(e.value) AS value FROM event e JOIN source s ON s.id = e.feed_id WHERE e.ts >= '2024-07-01' GROUP BY s.id, s.kind ORDER BY value DESC, s.id LIMIT 10;

-- query: silent-sources-in-month
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS silent FROM source s WHERE NOT EXISTS (SELECT 1 FROM event e WHERE e.feed_id = s.id AND e.ts >= '2024-03-01' AND e.ts < '2024-04-01');

-- query: latest-event-per-source-kind
-- kind: latest_per_group
-- join: inner
SELECT s.kind, max(e.ts) AS last_ts, count(*) AS n FROM event e JOIN source s ON s.id = e.feed_id GROUP BY s.kind;
