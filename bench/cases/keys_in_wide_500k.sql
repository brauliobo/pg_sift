-- the zone test of a long list is made once per scan and handed to the column layer
-- label: `wid IN (500,000 values)` over a 50-million-wide key, `count`, `avg` by `kind`
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_w e WHERE e.deleted_at IS NULL AND e.wid IN ({ids:keyset_w:500000}) GROUP BY 1 ORDER BY 1
