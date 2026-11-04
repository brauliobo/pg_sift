-- a request for one key of a lookup (a CTE of ids joined to the table) grouped over the members of a jsonb object: the planner prices the aggregate scan and the b-tree probe for the key and takes the cheaper, the scan up to 1,000,000 rows (a unit holds the key's rows), the b-tree probe where the key's rows are in most units
-- label: `WITH ids AS (one parent)`, `events JOIN ids ON ids.id = e.parent_id, jsonb_each(e.doc -> 'metrics')`, `count`, `avg` by member, a btree on `(parent_id, ts, deleted_at)`
-- ordered: yes
--@ pg
WITH ids AS MATERIALIZED (SELECT DISTINCT parent_id AS id FROM events WHERE id = 11)
SELECT r.key, count(*) AS n, avg((r.value #>> '{}')::float8) AS avg_value FROM events e JOIN ids ON ids.id = e.parent_id, jsonb_each(e.doc -> 'metrics') r
WHERE e.deleted_at IS NULL AND e.ts >= date '2025-01-01' GROUP BY 1 ORDER BY 1
