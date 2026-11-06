-- The groups of a key that is in a list: the condition with a sub-select of the HAVING is a condition of the rows, so the rows of the other keys are not grouped
-- label: `SELECT kind_id, count(*), sum(amount) FROM hw_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hw_kinds WHERE enabled)` over 300,000 rows, 1,000 keys and 100 of them in the list
-- ordered: no
--@ setup
CREATE TABLE hw_kinds AS SELECT g AS id, g % 10 = 0 AS enabled FROM generate_series(1, 1000) g;
ALTER TABLE hw_kinds ADD PRIMARY KEY (id);
CREATE TABLE hw_events AS SELECT g AS id, 1 + (g * 7) % 1000 AS kind_id, (((g::bigint * 37) % 1000) / 10.0)::float8 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX hw_events_col ON hw_events USING sift_col (id, kind_id, amount);
VACUUM (FREEZE, ANALYZE) hw_kinds, hw_events
--@ pg
SELECT kind_id, count(*), sum(amount) FROM hw_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hw_kinds WHERE enabled)
