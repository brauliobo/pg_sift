-- The lookup table of a semi join read through its btree for the bound of its own column: the 2,000 ids of a table of 200,000 rows that the events are joined to, not the whole table
-- label: `SELECT kind_id, count(*), sum(amount) FROM lb_events WHERE id_ref IN (SELECT id FROM lb_dims WHERE id <= 2000) GROUP BY 1` over 300,000 rows, a table of 200,000 ids and 2,000 of them in the bound
-- ordered: no
--@ setup
CREATE TABLE lb_dims AS SELECT g AS id, g % 7 AS grp FROM generate_series(1, 200000) g;
ALTER TABLE lb_dims ADD PRIMARY KEY (id);
CREATE TABLE lb_events AS SELECT g AS id, 1 + (g::bigint * 7919) % 200000 AS id_ref, g % 5 AS kind_id, (((g::bigint * 37) % 1000) / 10.0)::float8 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX lb_events_col ON lb_events USING sift_col (id, id_ref, kind_id, amount);
VACUUM (FREEZE, ANALYZE) lb_dims, lb_events
--@ pg
SELECT kind_id, count(*), sum(amount) FROM lb_events WHERE id_ref IN (SELECT id FROM lb_dims WHERE id <= 2000) GROUP BY 1
