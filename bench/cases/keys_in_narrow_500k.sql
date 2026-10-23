-- planning a long IN list repeats nothing of the planner's work
-- label: `item_id IN (500,000 values)` over a key a byte table covers, planning included
-- ordered: yes
--@ pg
SELECT kind, count(*), round(avg(score)::numeric, 3) FROM events_u e WHERE e.deleted_at IS NULL AND e.item_id IN ({ids:keyset:500000}) GROUP BY 1 ORDER BY 1
