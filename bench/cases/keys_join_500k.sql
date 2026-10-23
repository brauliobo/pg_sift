-- a lookup join loaded by the heap scan when its equality keeps most of the table: a join
-- label: `JOIN members m ON m.item_id = e.item_id AND m.group_id = 500000`, `count`, `avg` by `kind`
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_u e JOIN dim_b m ON m.item_id = e.item_id AND m.group_id = 500000 WHERE e.deleted_at IS NULL GROUP BY 1 ORDER BY 1
