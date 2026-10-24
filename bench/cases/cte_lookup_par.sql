-- a scan whose lookup joins read a CTE (rows the leader loads and shares) plans workers: one process before
-- label: `count(*), sum(amount)` of every order joined to a materialized CTE of the 6,313 segments, `GROUP BY` its 7 groups, 2 workers
-- ordered: yes
--@ pg
WITH ps AS MATERIALIZED (SELECT g AS id, g % 7 AS grp FROM generate_series(0, 6312) g)
SELECT ps.grp, count(*), sum(o.amount) FROM orders o JOIN ps ON ps.id = o.segment_id GROUP BY ps.grp ORDER BY ps.grp
