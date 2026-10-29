-- a scan of one leaf of a key-cut unit reads its head (2 kB, the chunk table first) and the chunk, not the directory of the rows (column format minor 34): a filter on a measure, summed by a group, with the pre-aggregates off
-- label: `SELECT country, count(*), sum(amount) WHERE quantity < 3 GROUP BY country`, `key_cut = 'customer_id'`, no btree, `sift.agg_preagg = off`
-- ordered: yes
-- set: sift.agg_preagg = off
--@ pg
SELECT country, count(*), sum(amount) FROM orders WHERE quantity < 3 GROUP BY country ORDER BY country
