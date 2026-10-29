-- a lookup table answered from a byte table of its keys, loaded by the first statement of a backend (the cache of lookup tables off): the table is read without the dictionary of its keys and the chains of its rows
-- label: `item_id IN (SELECT id FROM keyset WHERE n = 500000)`, `count`, `avg` by `kind`, the scan forced and the lookup cache off
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_u e WHERE e.deleted_at IS NULL AND e.item_id IN (SELECT id FROM keyset WHERE n = 500000) GROUP BY 1 ORDER BY 1
