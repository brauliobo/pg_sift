-- query: live-amount-by-group
-- kind: group_by_dim_attr
-- join: inner
SELECT d.grp, count(*) AS n, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id WHERE f.deleted_at IS NULL AND d.deleted_at IS NULL GROUP BY d.grp;

-- query: live-facts-of-deleted-dims
-- kind: filter_dim_attr
-- join: inner
SELECT d.state, count(*) AS n, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id WHERE f.deleted_at IS NULL AND d.deleted_at IS NOT NULL GROUP BY d.state;

-- query: distinct-live-dims
-- kind: count_distinct_across_join
-- join: inner
SELECT d.state, count(DISTINCT f.dim_id) AS dims FROM fact f JOIN dim d ON d.id = f.dim_id WHERE f.deleted_at IS NULL AND d.deleted_at IS NULL GROUP BY d.state;

-- query: deleted-share-by-state
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT d.state, count(*) FILTER (WHERE f.deleted_at IS NOT NULL) AS fact_deleted, count(*) FILTER (WHERE d.deleted_at IS NOT NULL) AS dim_deleted,
       count(*) FILTER (WHERE f.deleted_at IS NULL AND d.deleted_at IS NULL) AS live, count(*) AS n FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.state;

-- query: live-dims-without-live-facts
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS idle FROM dim d WHERE d.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM fact f WHERE f.dim_id = d.id AND f.deleted_at IS NULL);

-- query: top-live-dims
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT d.id, d.state, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id WHERE f.deleted_at IS NULL AND d.deleted_at IS NULL GROUP BY d.id, d.state ORDER BY amount DESC, d.id LIMIT 10;

-- query: live-facts-by-dim-liveness-left
-- kind: group_by_dim_attr
-- join: left
SELECT (d.deleted_at IS NULL) AS dim_live, count(*) AS n FROM fact f LEFT JOIN dim d ON d.id = f.dim_id AND d.deleted_at IS NULL WHERE f.deleted_at IS NULL GROUP BY 1;
