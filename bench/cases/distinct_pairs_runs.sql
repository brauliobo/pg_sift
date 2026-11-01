-- the pairs of group and value of a count(DISTINCT) that outgrow the hash memory are sorted by radix as 8-byte keys, in runs on disk past the memory: 1,000,000 rows of 1,000 groups and values over 2^31
-- label: `count(DISTINCT uid)` by 1,000 groups, the 10 largest (default `work_mem`, the sets outgrow it)
--@ setup
CREATE TABLE run_pairs AS SELECT id, (id % 1000)::int AS g, (hashint8extended(id, 7) & 2147483647)::int AS uid FROM orders;
CREATE INDEX run_pairs_col ON run_pairs USING sift_col (id, g, uid);
VACUUM (FREEZE, ANALYZE) run_pairs;
--@ pg
SELECT g, count(DISTINCT uid) AS u FROM run_pairs GROUP BY g ORDER BY u DESC, g LIMIT 10
