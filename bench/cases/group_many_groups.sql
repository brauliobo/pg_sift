-- a group by of an integer column with 1,500,000 groups over 3,000,000 rows: the tables of the group ids and of the groups are far bigger than the caches: the slots of the next rows are fetched while one is probed, a key that outgrows its arrays is not renumbered, and one key's groups stay in arrays past a million
-- label: `count(*)` and `sum(v)` by `grp` (1,500,000 groups), a table of 3,000,000 rows, at `work_mem = 1GB`
-- set: work_mem = '1GB'
--@ setup
CREATE TABLE many_groups AS SELECT g AS id, (hashint4(g) & 2147483647) % 1500000 AS grp, g % 1000 AS v FROM generate_series(1, 3000000) g;
CREATE INDEX many_groups_col ON many_groups USING sift_col (grp, v);
ANALYZE many_groups
--@ pg
SELECT sum(a + b) FROM (SELECT grp, count(*) AS a, sum(v) AS b FROM many_groups GROUP BY grp) s
