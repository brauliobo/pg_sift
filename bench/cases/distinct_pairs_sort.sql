-- the pairs of group and value of a count(DISTINCT) that outgrow the hash memory are sorted as int8 datums: 1,000,000 rows of 60,000 groups and as many users
-- label: `count(DISTINCT uid)` and `count(*)` by phrase over 60,000 phrases, the 10 largest (default `work_mem`, the sets spill)
--@ setup
CREATE TABLE pairs AS SELECT id, (hashint8extended(id, 1) & 4194303) AS uid, 'phrase ' || (hashint8extended(id, 2) & 65535) % 60000 AS phrase FROM orders;
CREATE INDEX pairs_col ON pairs USING sift_col (id, uid, phrase);
VACUUM (FREEZE, ANALYZE) pairs;
--@ pg
SELECT phrase, count(*) AS c, count(DISTINCT uid) AS u FROM pairs GROUP BY phrase ORDER BY c DESC, phrase LIMIT 10
