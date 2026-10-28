-- a rolling window anchored to a stable expression (a dashboard's `now() - interval`): the bounds are found when the scan starts, then the kernels compare them with the zones of the units
-- label: `count(*), sum(amount) WHERE created_at >= timestamptz '2025-06-01 00:00:00+00' - interval '30 days'`, a `timestamp` against a stable `timestamptz`
-- ordered: yes
--@ pg
SELECT count(*) AS n, sum(amount) AS total FROM orders WHERE created_at >= timestamptz '2025-06-01 00:00:00+00' - interval '30 days'
