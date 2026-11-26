-- a month report over a range of a table in four range partitions: the partitions the range leaves out are pruned, each one kept is aggregated by its own sift_col index
-- label: `GROUP BY date_trunc('month', ts)::date, kind` of `events_part` (four half-year partitions) between 2024-04-01 and 2025-10-01, the live rows
-- ordered: yes
--@ pg
SELECT date_trunc('month', ts)::date AS m, kind, count(*), sum(amount) FROM events_part
WHERE ts >= date '2024-04-01' AND ts < date '2025-10-01' AND deleted_at IS NULL GROUP BY 1, 2 ORDER BY 1, 2
