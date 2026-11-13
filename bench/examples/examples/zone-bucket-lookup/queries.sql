-- query: one-zone-by-hour
-- kind: lookup_exact_key
-- join: inner
SELECT z.kind, date_trunc('day', r.at) AS day, count(*) AS n, sum(r.level) AS level FROM reading r JOIN zone z ON z.id = r.zone_id WHERE r.zone_id = 'z-0000007' GROUP BY z.kind, 2;

-- query: five-zones-by-region
-- kind: lookup_exact_key
-- join: inner
SELECT z.region, count(*) AS n, sum(r.level) AS level FROM reading r JOIN zone z ON z.id = r.zone_id WHERE r.zone_id IN ('z-0000001', 'z-0000002', 'z-0000003', 'z-0000004', 'z-0000005') GROUP BY z.region;

-- query: readings-by-zone-kind
-- kind: group_by_dim_attr
-- join: inner
SELECT z.kind, count(*) AS n, sum(r.level) AS level FROM reading r JOIN zone z ON z.id = r.zone_id GROUP BY z.kind;

-- query: busy-zones-by-capacity
-- kind: filter_dim_attr
-- join: inner
SELECT z.region, count(*) AS n FROM reading r JOIN zone z ON z.id = r.zone_id WHERE z.capacity >= 900 AND r.level >= 90 GROUP BY z.region;

-- query: distinct-zones-by-region
-- kind: count_distinct_across_join
-- join: inner
SELECT z.region, count(DISTINCT r.zone_id) AS zones FROM reading r JOIN zone z ON z.id = r.zone_id GROUP BY z.region;

-- query: top-zones-by-level
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT z.id, z.kind, sum(r.level) AS level FROM reading r JOIN zone z ON z.id = r.zone_id GROUP BY z.id, z.kind ORDER BY level DESC, z.id LIMIT 10;

-- query: empty-zones
-- kind: anti_join_count
-- join: anti
SELECT z.region, count(*) AS empty FROM zone z WHERE NOT EXISTS (SELECT 1 FROM reading r WHERE r.zone_id = z.id) GROUP BY z.region;

-- query: peak-readings-by-kind
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT z.kind, count(*) FILTER (WHERE r.level >= 90) AS peaks, count(*) FILTER (WHERE z.region = 'r1') AS region1, count(*) AS n FROM reading r JOIN zone z ON z.id = r.zone_id GROUP BY z.kind;
