-- query: amount-by-group
-- kind: group_by_dim_attr
-- join: inner
SELECT d.grp, count(*) AS n, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.grp;

-- query: flagged-in-state
-- kind: filter_dim_attr
-- join: inner
SELECT count(*) AS n, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id WHERE d.state = 'w1' AND d.flag;

-- query: distinct-amounts-by-state
-- kind: count_distinct_across_join
-- join: inner
SELECT d.state, count(DISTINCT f.amount) AS amounts, count(DISTINCT f.dim_id) AS dims FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.state;

-- query: status-by-flag-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT f.status, count(*) FILTER (WHERE d.flag) AS flagged, sum(f.amount) FILTER (WHERE d.state IN ('w1', 'w2')) AS amount, count(*) AS n
FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY f.status;

-- query: top-dims-by-amount
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT d.id, d.state, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.id, d.state ORDER BY amount DESC, d.id LIMIT 10;

-- query: weighted-amount-by-state
-- kind: sum_avg
-- join: inner
SELECT d.state, sum(f.amount * d.weight) AS weighted, round(avg(d.weight), 4) AS avg_weight FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.state;

-- query: facts-by-state-left
-- kind: group_by_dim_attr
-- join: left
SELECT d.state, count(*) AS n, count(d.id) AS matched FROM fact f LEFT JOIN dim d ON d.id = f.dim_id GROUP BY d.state;

-- query: dims-without-facts
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS unused FROM dim d WHERE NOT EXISTS (SELECT 1 FROM fact f WHERE f.dim_id = d.id);

-- query: count-star-vs-count-key
-- kind: group_by_dim_attr
-- join: left
SELECT f.status, count(*) AS rows, count(f.dim_id) AS keyed, count(d.id) AS matched FROM fact f LEFT JOIN dim d ON d.id = f.dim_id GROUP BY f.status;

-- query: not-in-with-null-keys
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS n FROM fact f WHERE f.dim_id NOT IN (SELECT id FROM dim WHERE flag);

-- query: not-exists-with-null-keys
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS n FROM fact f WHERE NOT EXISTS (SELECT 1 FROM dim d WHERE d.id = f.dim_id AND d.flag);

-- query: missing-keys-by-status
-- kind: missing_field_count
-- join: left
SELECT f.status, count(*) FILTER (WHERE f.dim_id IS NULL) AS no_key, count(*) FILTER (WHERE f.dim_id IS NOT NULL AND d.id IS NULL) AS dangling, count(*) AS n FROM fact f LEFT JOIN dim d ON d.id = f.dim_id GROUP BY f.status;
