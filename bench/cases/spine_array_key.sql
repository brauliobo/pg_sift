-- a series of days LEFT JOINed on jsonb_build_array(..) = jsonb_build_array(..): the element is a hash key of the lookup
-- label: 701 days `LEFT JOIN events ON jsonb_build_array(e.ts) = jsonb_build_array(day)`, `count`
-- ordered: yes
--@ pg
SELECT g.d AS day, count(e.id) AS n FROM (SELECT d::date AS d FROM generate_series(date '2024-01-01', date '2025-12-01', interval '1 day') s(d) OFFSET 0) g
LEFT JOIN events e ON jsonb_build_array(e.ts) = jsonb_build_array(g.d) AND e.deleted_at IS NULL GROUP BY 1 ORDER BY 1
