-- Two series of buckets (a spine of weeks of two periods LEFT JOINed to the rows of each document by a jsonb array of keys) combined by a FULL JOIN on a jsonb array and a lag window.
-- The events CTE (base) is read by both series: PostgreSQL keeps it materialized; sift plans it inline (src/agg/cte.c). Run with sift.enable_agg on and off; 100k rows: about 2.3 s normal, 60 ms with both series as scans.
WITH periods AS (
  SELECT * FROM (VALUES ('cur', daterange(current_date - 364, current_date, '[]'), interval '0 day'),
                        ('prev', daterange(current_date - 729, current_date - 365, '[]'), interval '1 year')) p(period, range, shift)),
all_dates AS (
  SELECT period, (generate_series(date_trunc('week', lower(range)::date)::date + shift, date_trunc('week', (upper(range) - interval '1 day')::date)::date + shift, interval '1 week'))::date AS day
  FROM periods),
base AS (
  SELECT e.*, periods.period AS bucket_period, (date_trunc('week', e.ts)::date + periods.shift)::date AS bucket_date
  FROM events e JOIN periods ON e.ts <@ periods.range JOIN dims ON dims.id = e.parent_id AND dims.kind <> 'cat_d'
  WHERE e.deleted_at IS NULL),
parts AS (
  SELECT b.id, b.parent_id, b.bucket_period, b.bucket_date, o.cat AS cat, o.sub AS sub, avg(o.score) AS score
  FROM base b CROSS JOIN LATERAL jsonb_to_recordset(b.payload['parts']) o(cat text, sub text, score float8)
  WHERE o.score IS NOT NULL
  GROUP BY b.id, b.parent_id, b.bucket_period, b.bucket_date, o.cat, o.sub),
metrics AS (
  SELECT b.id, b.parent_id, b.bucket_period, b.bucket_date, coalesce(m.cat, split_part(r.key, '_', 1)) AS cat, coalesce(m.sub, nullif(split_part(r.key, '_', 2), '')) AS sub, r.value::float8 AS score
  FROM base b CROSS JOIN LATERAL jsonb_each(b.metrics) r LEFT JOIN maps m ON m.src = r.key),
a AS (
  SELECT coalesce(cat, 'mhead') AS cat, sub, day, period, avg(score) AS value, count(DISTINCT id) AS n, count(DISTINCT parent_id) AS parents
  FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN parts x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
  GROUP BY GROUPING SETS ((cat, sub, day, period))),
b AS (
  SELECT coalesce(cat, 'mhead') AS cat, sub, day, period, avg(score) AS value, count(DISTINCT id) AS n, count(DISTINCT parent_id) AS parents
  FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN metrics x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
  GROUP BY GROUPING SETS ((cat, sub, day, period))),
g AS (
  SELECT coalesce(a.cat, b.cat) AS cat, coalesce(a.sub, b.sub) AS sub, coalesce(a.day, b.day) AS day, coalesce(a.period, b.period) AS period,
         coalesce(a.value, b.value) AS value, coalesce(b.n, a.n) AS n, a.value AS a_value, b.value AS b_value
  FROM a FULL JOIN b ON jsonb_build_array(a.cat, a.sub, a.day, a.period) = jsonb_build_array(b.cat, b.sub, b.day, b.period)),
t AS (
  SELECT g.*, lag(g.value) OVER w AS prev, lag(g.n) OVER w AS prev_n, lag(g.a_value) OVER w AS prev_a, lag(g.b_value) OVER w AS prev_b
  FROM g WINDOW w AS (PARTITION BY day, cat, sub ORDER BY period DESC))
SELECT * FROM t WHERE period = 'cur' ORDER BY (cat = 'mhead') DESC, day, cat, sub
;
