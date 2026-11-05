-- an aggregate of a column of the scanned table under a RIGHT JOIN to a lookup: the column's own kernels, not an id in a hash table for each distinct value of it
-- label: `count(f.id)` by a column of the lookup, `FROM fact f RIGHT JOIN dim ON dim.id = f.dim_id`, 600,000 rows and a lookup of 60,000
-- ordered: yes
--@ setup
CREATE TABLE gr_dim AS SELECT g AS id, g % 11 AS grp FROM generate_series(1, {rows} / 10) g;
ALTER TABLE gr_dim ADD PRIMARY KEY (id);
CREATE TABLE gr_fact AS SELECT g AS id, (g::bigint * 104729 % ({rows} / 10))::int + 1 AS dim_id, g % 97 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX gr_fact_col ON gr_fact USING sift_col (id, dim_id, amount);
ANALYZE gr_dim;
ANALYZE gr_fact
--@ pg
SELECT d.grp, count(f.id) AS n, sum(f.amount) AS s FROM gr_fact f RIGHT JOIN gr_dim d ON d.id = f.dim_id GROUP BY d.grp ORDER BY d.grp
