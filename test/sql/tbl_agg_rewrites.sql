-- The rewrites that read a table with a column index read a sift_table too: a LATERAL sub-select that aggregates, the comma form, and EXISTS / NOT EXISTS
-- of the table tied to the outer rows are the table grouped by the tie column, left joined to the outer rows. The answers are the normal plan's.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE key_sets (id int PRIMARY KEY, k int);
INSERT INTO key_sets SELECT i, i % 7 FROM generate_series(1, 2000) i;
CREATE TABLE items (id int, set_id int, cat int, amt int, d date) USING sift_table;
INSERT INTO items SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 1200 END, i % 9, (i * 7) % 100, date '2024-01-01' + i % 300 FROM generate_series(1, 30000) i;
CREATE INDEX items_owner ON items (set_id);
VACUUM (FREEZE, ANALYZE) key_sets;
VACUUM (FREEZE, ANALYZE) items;

\ir test/agg_rows_helpers.sql

CREATE TABLE rewrite_q (name text, q text);
INSERT INTO rewrite_q VALUES
	('lateral_group', 'SELECT o.id, l.cat, l.n FROM key_sets o JOIN LATERAL (SELECT cat, sum(amt) AS n FROM items i WHERE i.set_id = o.id GROUP BY cat) l ON true'),
	('lateral_comma', 'SELECT o.id, l.n FROM key_sets o, LATERAL (SELECT sum(amt) AS n FROM items i WHERE i.set_id = o.id) l'),
	('exists_select', 'SELECT o.id, EXISTS (SELECT 1 FROM items f WHERE f.set_id = o.id AND f.amt > 90) AS big FROM key_sets o'),
	('not_exists_or', 'SELECT o.id, (o.id < 100 OR NOT EXISTS (SELECT 1 FROM items f WHERE f.set_id = o.id AND f.amt > 90)) AS keep FROM key_sets o');

SELECT name, agg_pushed(q) AS pushed, agg_rows(q, true) = agg_rows(q, false) AS same FROM rewrite_q ORDER BY name;

-- rows in the write buffer and tombstones
INSERT INTO items SELECT 100000 + i, i % 1200, i % 9, i % 100, date '2024-06-01' FROM generate_series(1, 500) i;
DELETE FROM items WHERE id % 11 = 0;
SELECT name, agg_pushed(q) AS pushed, agg_rows(q, true) = agg_rows(q, false) AS same FROM rewrite_q ORDER BY name;

DROP TABLE rewrite_q, items, key_sets;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool);
DROP EXTENSION pg_sift;
