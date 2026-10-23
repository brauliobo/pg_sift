-- a long IN list over a range wider than a byte table is tested by a bit table
-- label: `wid IN (50,000 values)` over a 50-million-wide key, `count`, `avg` by `kind`
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_w e WHERE e.deleted_at IS NULL AND e.wid IN ({ids:keyset_w:50000}) GROUP BY 1 ORDER BY 1
