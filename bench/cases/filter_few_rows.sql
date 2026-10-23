-- filters that keep under a 32nd of a unit's rows (three parents of 400, spread over every unit): the unit goes on as those rows alone
-- label: `WHERE parent_id IN (7, 8, 9) GROUP BY kind` (0.75% of the rows), a btree on `parent_id`
-- ordered: yes
--@ pg
SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id IN (7, 8, 9) AND deleted_at IS NULL GROUP BY 1 ORDER BY 1
