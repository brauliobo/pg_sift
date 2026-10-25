-- count(DISTINCT) of two columns of a joined table, per group, over 6,000,000 rows and 600,000 groups: the (group, value) pairs fill a hash set larger than the caches, the slots of the next pairs are fetched while one is added
-- label: `count(DISTINCT cat)` and `count(DISTINCT lang)` of a lookup table of 50,000 rows by `grp` (600,000 groups of 10 rows), a table of 6,000,000 rows
-- ordered: yes
--@ setup
CREATE TABLE pair_dim AS SELECT g AS id, (g * 7919 % 300)::int AS cat, (g % 12)::int AS lang FROM generate_series(1, 50000) g;
ALTER TABLE pair_dim ADD PRIMARY KEY (id);
CREATE TABLE pair_fact AS SELECT g AS id, (g::bigint * 104729 % 50000)::int + 1 AS dim_id, (g % 600000)::int AS grp FROM generate_series(1, 6000000) g;
CREATE INDEX pair_fact_col ON pair_fact USING sift_col (dim_id, grp);
ANALYZE pair_dim;
ANALYZE pair_fact
--@ pg
SELECT f.grp, count(DISTINCT d.cat), count(DISTINCT d.lang) FROM pair_fact f JOIN pair_dim d ON d.id = f.dim_id GROUP BY 1 ORDER BY 1 LIMIT 1000
