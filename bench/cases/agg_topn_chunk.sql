-- the five best rows of each group (array_agg ORDER BY ... [1:5]) with two sort keys: the tuples that come after the group's last kept one are dropped on the first key alone
-- label: `SELECT kind_id, (array_agg(id ORDER BY amount DESC NULLS LAST, id))[1:5] FROM tk_events GROUP BY 1` over 300,000 rows and 20 groups
-- ordered: no
--@ setup
CREATE TABLE tk_events AS SELECT g AS id, 1 + g % 20 AS kind_id, (((g::bigint * 7919) % 100003) / 10.0)::float8 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX tk_events_col ON tk_events USING sift_col (id, kind_id, amount);
VACUUM (FREEZE, ANALYZE) tk_events
--@ pg
SELECT kind_id, (array_agg(id ORDER BY amount DESC NULLS LAST, id))[1:5] FROM tk_events GROUP BY 1
