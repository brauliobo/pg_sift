-- the predicate of the index as the only condition, a global aggregate
-- shape: flat
-- expect: agg
-- key: -
SELECT count(*) AS n, min(day) AS first_day, max(day) AS last_day, count(amount) AS priced FROM t_partial WHERE deleted_at IS NULL AND day >= '2026-01-08'
