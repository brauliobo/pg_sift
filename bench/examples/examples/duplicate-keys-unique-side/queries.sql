-- query: rows-and-facts-by-group
-- kind: group_by_dim_attr
-- join: inner
SELECT d.grp, count(*) AS joined_rows, count(DISTINCT f.id) AS facts, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.grp;

-- query: fan-out-by-version
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT d.version, count(*) AS joined_rows, count(DISTINCT f.dim_id) AS dims FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.version;

-- query: duplicated-keys
-- kind: self_join
-- join: inner
SELECT count(DISTINCT a.id) AS duplicated_keys FROM dim a JOIN dim b ON b.id = a.id AND b.version > a.version;

-- query: facts-of-duplicated-keys
-- kind: filter_dim_attr
-- join: semi
SELECT count(*) AS facts, sum(f.amount) AS amount FROM fact f WHERE f.dim_id IN (SELECT id FROM dim WHERE version > 1);

-- query: facts-with-latest-version
-- kind: latest_per_group
-- join: inner
-- oracle: SELECT d.state, count(*) AS facts, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id JOIN (SELECT id, max(version) AS version FROM dim GROUP BY id) m ON m.id = d.id AND m.version = d.version GROUP BY d.state
SELECT d.state, count(*) AS facts, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id
WHERE d.version = (SELECT max(x.version) FROM dim x WHERE x.id = d.id) GROUP BY d.state;

-- query: keys-without-facts
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS idle FROM dim d WHERE NOT EXISTS (SELECT 1 FROM fact f WHERE f.dim_id = d.id);

-- query: facts-by-state-left
-- kind: group_by_dim_attr
-- join: left
SELECT d.state, count(*) AS joined_rows, count(f.id) AS facts FROM dim d LEFT JOIN fact f ON f.dim_id = d.id GROUP BY d.state;

-- query: top-keys-by-joined-rows
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT d.id, count(*) AS joined_rows, sum(f.amount) AS amount FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY d.id ORDER BY joined_rows DESC, d.id LIMIT 10;
