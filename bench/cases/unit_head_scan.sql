-- a scan of few leaves of a unit reads the first 4 kB of the unit and the chunks it needs, not 24 kB of its head: a filter on a measure, summed by a group, with the pre-aggregates off
-- label: `SELECT country, count(*), sum(amount) WHERE quantity < 3 GROUP BY country`, no btree, `sift.agg_preagg = off`
-- ordered: yes
-- set: sift.agg_preagg = off
--@ pg
SELECT country, count(*), sum(amount) FROM orders WHERE quantity < 3 GROUP BY country ORDER BY country
