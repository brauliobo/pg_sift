-- query: amount-by-label
-- kind: group_by_dim_attr
-- join: inner
SELECT s.label, count(*) AS n, sum(f.amount) AS amount FROM fact f JOIN status s ON s.code = f.status_code GROUP BY s.label;

-- query: final-facts-by-month
-- kind: filter_dim_attr
-- join: inner
SELECT date_trunc('month', f.at) AS month, count(*) AS n FROM fact f JOIN status s ON s.code = f.status_code WHERE s.is_final GROUP BY 1;

-- query: distinct-keepers-by-label
-- kind: count_distinct_across_join
-- join: inner
SELECT s.label, count(DISTINCT f.owner) AS keepers FROM fact f JOIN status s ON s.code = f.status_code GROUP BY s.label;

-- query: unknown-status-left
-- kind: anti_join_count
-- join: left
SELECT count(*) AS unknown, sum(f.amount) AS amount FROM fact f LEFT JOIN status s ON s.code = f.status_code WHERE s.code IS NULL;

-- query: statuses-by-rank-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT s.rank, count(*) FILTER (WHERE f.amount >= 500) AS big, sum(f.amount) FILTER (WHERE s.is_final) AS final_amount, count(*) AS n FROM fact f JOIN status s ON s.code = f.status_code GROUP BY s.rank;

-- query: top-keepers-in-open
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT f.owner, sum(f.amount) AS amount FROM fact f JOIN status s ON s.code = f.status_code WHERE s.rank <= 2 GROUP BY f.owner ORDER BY amount DESC, f.owner LIMIT 10;

-- query: statuses-without-facts-right
-- kind: group_by_dim_attr
-- join: right
SELECT s.code, count(f.id) AS facts FROM fact f RIGHT JOIN status s ON s.code = f.status_code GROUP BY s.code;
