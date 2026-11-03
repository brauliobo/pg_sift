-- the sets of a count(DISTINCT) whose values are spread over a wide domain, a few in each group, are hash sets and not bitmaps of a chunk and a directory for each value: 1,000,000 rows of 60,000 groups and 4,000,000 values
-- label: `count(DISTINCT uid)` and `count(*)` by phrase over 60,000 phrases, the 10 largest (`work_mem = 256MB`, the sets fit)
-- set: work_mem = '256MB'
--@ setup
CREATE TABLE sparse_pairs AS SELECT id, (hashint8extended(id, 1) & 4194303) AS uid, 'phrase ' || (hashint8extended(id, 2) & 65535) % 60000 AS phrase FROM orders;
CREATE INDEX sparse_pairs_col ON sparse_pairs USING sift_col (id, uid, phrase);
VACUUM (FREEZE, ANALYZE) sparse_pairs;
--@ pg
SELECT phrase, count(*) AS c, count(DISTINCT uid) AS u FROM sparse_pairs GROUP BY phrase ORDER BY c DESC, phrase LIMIT 10
