-- a short IN list over a range a byte table covers: the table is priced, so the index plan that reads a few rows is kept
-- label: `item_id IN (256 values)` over a key of a million values, `count`, `avg` by `kind`
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_u e WHERE e.deleted_at IS NULL AND e.item_id IN ({ids:keyset:256}) GROUP BY 1 ORDER BY 1
