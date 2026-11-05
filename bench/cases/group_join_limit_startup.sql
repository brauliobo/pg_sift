-- the first groups of a grouped sub-select joined to a dimension on the group key, with a LIMIT: the plan that gives its first group at once stays a candidate next to the scan, which reads every unit before its first
-- label: `ORDER BY f.grp LIMIT 10` over `(SELECT grp, count(*) FROM fact GROUP BY grp) JOIN dim ON dim.id = grp`, 600,000 groups of 10 rows
-- ordered: yes
--@ setup
CREATE TABLE gj_dim AS SELECT g AS id, 'name_' || g AS name FROM generate_series(1, {rows} * 3 / 5) g;
ALTER TABLE gj_dim ADD PRIMARY KEY (id);
CREATE TABLE gj_fact AS SELECT g AS id, (g::bigint * 104729 % ({rows} * 3 / 5))::int + 1 AS grp, g % 97 AS amount FROM generate_series(1, {rows} * 6) g;
CREATE INDEX gj_fact_grp ON gj_fact (grp);
CREATE INDEX gj_fact_col ON gj_fact USING sift_col (id, grp, amount);
ANALYZE gj_dim;
ANALYZE gj_fact
--@ pg
SELECT d.name, a.n FROM (SELECT grp, count(*) AS n FROM gj_fact GROUP BY grp) a JOIN gj_dim d ON d.id = a.grp ORDER BY a.grp LIMIT 10
