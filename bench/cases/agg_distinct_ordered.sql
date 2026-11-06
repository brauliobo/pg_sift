-- the distinct values of a group ordered by the argument itself (array_agg(DISTINCT x ORDER BY x)) are a set that is sorted once, where every row of the group went into a sort
-- label: `SELECT region, array_agg(DISTINCT kind_id ORDER BY kind_id) FROM do_events GROUP BY 1` over 300,000 rows, 12 regions and 20 kinds
-- ordered: no
--@ setup
CREATE TABLE do_events AS SELECT g AS id, 1 + (g * 7) % 20 AS kind_id, 'region_' || (1 + (g * 11) % 12) AS region FROM generate_series(1, {rows}) g;
CREATE INDEX do_events_col ON do_events USING sift_col (id, kind_id, region);
VACUUM (FREEZE, ANALYZE) do_events
--@ pg
SELECT region, array_agg(DISTINCT kind_id ORDER BY kind_id) FROM do_events GROUP BY 1
