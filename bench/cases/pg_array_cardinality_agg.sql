-- a text[] column tested by containment and by its length under a grouped aggregate: the lengths are kept beside the images of the arrays, against a GIN of the array
-- label: `GROUP BY status` of `tags @> ARRAY['t_1'] AND cardinality(tags) >= 2`, `avg(cardinality(tags))` over {rows} rows of one to four tags; the variant `gin` is a copy of the rows with a GIN on tags
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS arr_events;
CREATE TABLE arr_events (id int NOT NULL, status text NOT NULL, amount float8 NOT NULL, tags text[] NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO arr_events SELECT i, 'st_' || chr(97 + i % 8), 1 + (i % 17) + 0.25 * (i % 4),
	ARRAY(SELECT 't_' || ((i::bigint * 7919 + k * 31) % 40) FROM generate_series(1, 1 + i % 4) k) FROM generate_series(1, {rows}) i;
CREATE INDEX arr_events_col ON arr_events USING sift_col (id, status, amount, tags);
VACUUM (FREEZE, ANALYZE) arr_events;
DROP SCHEMA IF EXISTS c_pg_array_cardinality_agg__gin CASCADE;
CREATE SCHEMA c_pg_array_cardinality_agg__gin;
CREATE TABLE c_pg_array_cardinality_agg__gin.arr_events (LIKE arr_events) WITH (autovacuum_enabled = false);
INSERT INTO c_pg_array_cardinality_agg__gin.arr_events SELECT * FROM arr_events;
CREATE INDEX arr_events_tags ON c_pg_array_cardinality_agg__gin.arr_events USING gin (tags);
VACUUM (FREEZE, ANALYZE) c_pg_array_cardinality_agg__gin.arr_events
--@ pg
SELECT status, count(*), avg(cardinality(tags)) FROM arr_events WHERE tags @> ARRAY['t_1'] AND cardinality(tags) >= 2 GROUP BY 1 ORDER BY 1
