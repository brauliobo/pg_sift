-- the keys of a filtered dimension joined to a grouped sub-select are looked up before the groups are made: one name asks the fact table for one group, not for all of them
-- label: `WHERE d.name = '..'` over `(SELECT grp, count(*) FROM fact GROUP BY grp) JOIN dim ON dim.id = grp`, groups of 10 rows
-- ordered: yes
--@ setup
CREATE TABLE gk_dim AS SELECT g AS id, 'name_' || g AS name FROM generate_series(1, {rows} * 3 / 5) g;
ALTER TABLE gk_dim ADD PRIMARY KEY (id);
CREATE UNIQUE INDEX gk_dim_name ON gk_dim (name);
CREATE TABLE gk_fact AS SELECT g AS id, (g::bigint * 104729 % ({rows} * 3 / 5))::int + 1 AS grp, g % 97 AS amount FROM generate_series(1, {rows} * 6) g;
CREATE INDEX gk_fact_grp ON gk_fact (grp);
CREATE INDEX gk_fact_col ON gk_fact USING sift_col (id, grp, amount);
-- vacuumed, as a table with autovacuum is: the visibility map lets the normal plan read the index alone (without it that lane reads the heap)
VACUUM (FREEZE, ANALYZE) gk_dim;
VACUUM (FREEZE, ANALYZE) gk_fact
--@ pg
SELECT d.name, a.n FROM (SELECT grp, count(*) AS n FROM gk_fact GROUP BY grp) a JOIN gk_dim d ON d.id = a.grp WHERE d.name = 'name_15000'
