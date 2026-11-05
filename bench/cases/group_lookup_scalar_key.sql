-- a scalar sub-select that looks a row up by its unique key, in the argument of an aggregate of a grouped query: a left join the scan reads in its pass, not one probe for each row
-- label: `sum((SELECT d.w FROM dim d WHERE d.id = f.dim_id)) GROUP BY f.grp` over 600,000 rows and a dimension of 60,000
-- ordered: yes
--@ setup
CREATE TABLE gl_dim AS SELECT g AS id, g % 11 AS w, 'name_' || g AS name FROM generate_series(1, {rows} / 10) g;
ALTER TABLE gl_dim ADD PRIMARY KEY (id);
CREATE TABLE gl_fact AS SELECT g AS id, (g::bigint * 104729 % ({rows} / 10))::int + 1 AS dim_id, g % 17 AS grp FROM generate_series(1, {rows}) g;
CREATE INDEX gl_fact_col ON gl_fact USING sift_col (id, dim_id, grp);
ANALYZE gl_dim;
ANALYZE gl_fact
--@ pg
SELECT grp, sum((SELECT d.w FROM gl_dim d WHERE d.id = f.dim_id)) AS s FROM gl_fact f GROUP BY grp ORDER BY grp
