-- The rewrite that keeps the found flags of a connected index near their parents (docs/COL-CONNECT.md): a segment is rewritten when it was never filled, when its parent index was
-- rebuilt, or when the keys changed since its stamp are sift.col_connect_refresh_pct percent of the parent's rows; a few changed keys are left to the reads.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, label text, price int) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, 'l' || i, i * 10 FROM generate_series(1, 200) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, label, price);
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 200, i FROM generate_series(1, 2000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, v) WITH (connect = 'dim_a_id -> dim_a(id)');

-- rows flagged found whose parent row is not in the heap of the parent now (the flag is as of the stamp)
CREATE FUNCTION found_wrong() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM sift._test_col_read('events_col', 3) f
		JOIN events e ON e.ctid = f.tid
		LEFT JOIN dim_a d ON d.id = e.dim_a_id
		WHERE f.val IS NOT NULL AND d.id IS NULL
$$;
CREATE FUNCTION found_filled() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(val) FROM sift._test_col_read('events_col', 3)
$$;

-- never filled: the journal started after the first scan
SELECT sift.col_maintenance_due('events_col', false) AS due, found_filled() AS filled;
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT sift.col_maintenance_due('events_col', false) AS due, found_filled() AS filled, found_wrong() AS wrong;
SELECT count(*) AS segments, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM sift._test_col_conn_stamps('events_col');

-- a few changed keys are left to the reads
UPDATE dim_a SET id = id + 1000 WHERE id <= 5;
SELECT sift.col_maintenance_due('events_col', false) AS due, found_wrong() > 0 AS stale_values;

-- changed keys at the share of the setting
UPDATE dim_a SET id = id + 1000 WHERE id <= 30;
SELECT sift.col_maintenance_due('events_col', false) AS due;
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT sift.col_maintenance_due('events_col', false) AS due, found_filled() AS filled, found_wrong() AS wrong;
SELECT count(*) AS segments, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM sift._test_col_conn_stamps('events_col');

-- a lower share
SET sift.col_connect_refresh_pct = 1;
UPDATE dim_a SET id = id + 1000 WHERE id IN (100, 101, 102);
SELECT sift.col_maintenance_due('events_col', false) AS due;
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT sift.col_maintenance_due('events_col', false) AS due, found_wrong() AS wrong;
RESET sift.col_connect_refresh_pct;

-- the parent index rebuilt: another journal, so every stamp is old
REINDEX INDEX dim_a_col;
SELECT sift.col_maintenance_due('events_col', false) AS due;
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT sift.col_maintenance_due('events_col', false) AS due, found_wrong() AS wrong;

-- a parent whose keys do not fit maintenance_work_mem is looked up by its btree: filled by the first rewrite, nothing due after it
CREATE TABLE dim_big (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim_big SELECT i * 1000, repeat('x', 40) || i FROM generate_series(1, 60000) i;
CREATE INDEX dim_big_col ON dim_big USING sift_col (id, label) WITH (order_array_columns = 'off');
CREATE TABLE big_events (id int PRIMARY KEY, dim_big_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO big_events SELECT i, 1000 * (1 + i % 60000) FROM generate_series(1, 150000) i;
CREATE INDEX big_events_col ON big_events USING sift_col (id, dim_big_id) WITH (connect = 'dim_big_id -> dim_big(id)', key_cut = 'off', order_array_columns = 'off');
SET maintenance_work_mem = '1MB';
SELECT sift.col_maintenance_due('big_events_col', false) AS due;
SELECT sift.col_maintain('big_events_col') AS rewritten;
SELECT sift.col_maintenance_due('big_events_col', false) AS due;
RESET maintenance_work_mem;

DROP TABLE big_events, dim_big, events, dim_a CASCADE;
DROP FUNCTION found_wrong(), found_filled();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
