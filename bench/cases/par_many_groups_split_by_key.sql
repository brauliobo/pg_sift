-- a scan with workers of thousands of groups splits the groups by their key, not the rows: the tables of the participants are disjoint
-- label: `count(*), sum(v)` by `grp` (100,000 groups), a table of 3,000,000 rows, 4 workers (`--cpus 5`), the pre-aggregates off and `work_mem = 256MB`; `one_process` runs it with 0 workers
-- set: work_mem = '256MB'; sift.agg_preagg = off
--@ setup
CREATE TABLE split_groups WITH (parallel_workers = 4) AS SELECT g AS id, (hashint4(g) & 2147483647) % ({rows} / 10) AS grp, g % 1000 AS v FROM generate_series(1, {rows} * 3) g;
CREATE INDEX split_groups_col ON split_groups USING sift_col (id, grp, v) WITH (key_cut = off);
VACUUM (FREEZE, ANALYZE) split_groups
--@ pg
SELECT grp, count(*), sum(v) FROM split_groups GROUP BY grp
