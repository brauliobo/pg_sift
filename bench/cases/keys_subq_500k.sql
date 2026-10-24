-- a lookup join loaded by the heap scan when its equality keeps most of the table: IN (SELECT ..)
-- label: `item_id IN (SELECT id FROM keyset WHERE n = 500000)`, `count`, `avg` by `kind`
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_u e WHERE e.deleted_at IS NULL AND e.item_id IN (SELECT id FROM keyset WHERE n = 500000) GROUP BY 1 ORDER BY 1
