-- a lookup join loaded by the heap scan when its equality keeps most of the table: EXISTS
-- label: `EXISTS (SELECT 1 FROM members m WHERE m.item_id = e.item_id AND m.group_id = 500000 AND m.deleted_at IS NULL)`
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_u e WHERE e.deleted_at IS NULL AND EXISTS (SELECT 1 FROM dim_b m WHERE m.item_id = e.item_id AND m.group_id = 500000 AND m.deleted_at IS NULL) GROUP BY 1 ORDER BY 1
