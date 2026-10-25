-- a join to a lookup table of 1.5 million rows that the scan would read whole, from an outer side of 150,000 rows: the planner keeps the sequential scan PostgreSQL runs
-- label: `count(*), sum(amount)` of the first 150,000 orders joined to a table of 1,500,000 rows by primary key, `GROUP BY` its 50 groups
-- ordered: yes
--@ setup
CREATE TABLE lookup AS SELECT g AS id, (g % 50)::int AS grp FROM generate_series(1, 1500000) g;
ALTER TABLE lookup ADD PRIMARY KEY (id);
ANALYZE lookup
--@ pg
SELECT l.grp, count(*), sum(o.amount) FROM orders o JOIN lookup l ON l.id = o.id WHERE o.id <= 150000 GROUP BY l.grp ORDER BY l.grp
