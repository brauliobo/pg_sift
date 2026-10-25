-- the elements of a list tested for a member that is there, and counted: `p ->> 'score' IS NOT NULL` and `count(p ->> 'score')` read the typed leaf of the member
-- label: `jsonb_array_elements(doc -> 'parts')` with `p ->> 'score' IS NOT NULL`, `count(p ->> 'score')` and `avg((p ->> 'score')::float8)`
-- ordered: yes
--@ pg
SELECT count(p ->> 'score') AS scored, count(*) AS parts, avg((p ->> 'score')::float8) AS mean FROM events e, jsonb_array_elements(e.doc -> 'parts') p
WHERE p ->> 'score' IS NOT NULL AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01'
