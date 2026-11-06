-- A sub-select in the select list, correlated to the group key: the scan keeps the grouping and the node evaluates the sub-select once for each group, as it is above the aggregate
-- label: `SELECT kind_id, count(*), sum(amount), (SELECT name FROM ou_kinds k WHERE k.id = ou_events.kind_id) FROM ou_events GROUP BY 1` over 300,000 rows and 1,000 groups
-- ordered: no
--@ setup
CREATE TABLE ou_kinds AS SELECT g AS id, 'kind_' || g AS name FROM generate_series(1, 1000) g;
ALTER TABLE ou_kinds ADD PRIMARY KEY (id);
CREATE TABLE ou_events AS SELECT g AS id, 1 + (g * 7) % 1000 AS kind_id, (((g::bigint * 37) % 1000) / 10.0)::float8 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX ou_events_col ON ou_events USING sift_col (id, kind_id, amount);
VACUUM (FREEZE, ANALYZE) ou_kinds, ou_events
--@ pg
SELECT kind_id, count(*), sum(amount), (SELECT name FROM ou_kinds k WHERE k.id = ou_events.kind_id) FROM ou_events GROUP BY 1
