-- two groupings under UNION ALL, ORDER BY and LIMIT, one of them filtered by a column that moves with the group key: the normal plan for the second would read every group the filter drops before its first, which the statistics do not tell
-- label: `UNION ALL` of `count(*) .. WHERE band < 4 GROUP BY grp` and `.. WHERE band >= 4 GROUP BY grp`, `ORDER BY grp, n LIMIT 50`, 600,000 groups, band = grp / (groups / 10)
-- ordered: yes
--@ setup
CREATE TABLE gu_fact AS SELECT g AS id, (g::bigint * 104729 % ({rows} * 3 / 5))::int + 1 AS grp FROM generate_series(1, {rows} * 6) g;
ALTER TABLE gu_fact ADD COLUMN band int;
UPDATE gu_fact SET band = 1 + (grp - 1) / ({rows} * 3 / 50);
CREATE INDEX gu_fact_grp ON gu_fact (grp);
CREATE INDEX gu_fact_col ON gu_fact USING sift_col (id, grp, band);
VACUUM ANALYZE gu_fact
--@ pg
SELECT grp, n FROM (SELECT grp, count(*) AS n FROM gu_fact WHERE band < 4 GROUP BY grp UNION ALL SELECT grp, count(*) FROM gu_fact WHERE band >= 4 GROUP BY grp) u ORDER BY 1, 2 LIMIT 50
