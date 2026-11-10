-- the predicate of the index as the only condition, a global aggregate
-- shape: flat
-- expect: agg
-- key: -
SELECT count(*) AS n, min(id) AS first_id, max(id) AS last_id, count(amount) AS priced FROM t_partial_hidden WHERE status = 'open'
