-- Every kind and region with its count: a table left-joined to the join of two small tables, which one preserved lookup cannot be, so the scan makes the groups of the rows by the two keys and the query joins them to the pairs
-- label: `SELECT k.name, r.region, count(e.id) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2` over 300,000 events, 20 kinds and 12 regions
-- ordered: no
--@ setup
CREATE TABLE lp_kinds AS SELECT g AS id, 'kind_' || g AS name FROM generate_series(1, 20) g;
CREATE TABLE lp_regions AS SELECT 'region_' || g AS region FROM generate_series(1, 12) g;
CREATE TABLE lp_events AS SELECT g AS id, 1 + (g * 7) % 20 AS kind_id, 'region_' || (1 + (g * 11) % 12) AS region, ((g * 37) % 1000) / 10.0 AS amount FROM generate_series(1, {rows}) g;
ALTER TABLE lp_kinds ADD PRIMARY KEY (id);
ALTER TABLE lp_events ADD PRIMARY KEY (id);
CREATE INDEX lp_events_col ON lp_events USING sift_col (id, kind_id, region, amount);
VACUUM ANALYZE lp_kinds, lp_regions, lp_events
--@ pg
SELECT k.name, r.region, count(e.id) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2
