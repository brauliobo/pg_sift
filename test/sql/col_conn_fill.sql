-- The carried leaves of a connected index are filled when a segment is written (build, promotion, merge), from the parent rows visible to a snapshot, and the segment keeps
-- the stamp of that snapshot (docs/COL-CONNECT.md). What a segment holds is then as of its stamp; the parent keys that changed since are in the journal of the parent index.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, label text, price int) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, 'l' || i, i * 10 FROM generate_series(1, 100) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, label, price);
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 100, i FROM generate_series(1, 1000) i;
INSERT INTO events VALUES (1001, NULL, 1001), (1002, 999, 1002);
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, v) WITH (connect = 'dim_a_id -> dim_a(id) carries label, price');

-- rows whose carried values differ from the parent row of the index's own heap (a value the segment lacks is not counted: it is read by the join)
CREATE FUNCTION carried_wrong() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM sift._test_col_read('events_col', 4) l
		JOIN sift._test_col_read('events_col', 5) p USING (tid)
		JOIN events e ON e.ctid = l.tid
		LEFT JOIN dim_a d ON d.id = e.dim_a_id
		WHERE l.val IS NOT NULL AND (l.val IS DISTINCT FROM d.label OR p.val IS DISTINCT FROM d.price::text)
$$;
CREATE FUNCTION carried_filled() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(val) FROM sift._test_col_read('events_col', 3)
$$;

-- the journal of the parent starts after the first build: its segments say nothing
SELECT count(*) AS stamps, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM sift._test_col_conn_stamps('events_col');
SELECT carried_filled() AS filled, carried_wrong() AS wrong;

-- a promotion fills its segment under a stamp of the journal
INSERT INTO events SELECT i, 1 + i % 100, i FROM generate_series(2001, 2300) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
CREATE TEMP TABLE st1 AS SELECT * FROM sift._test_col_conn_stamps('events_col') WHERE lineage <> 0;
SELECT count(*) AS stamped_segments, count(DISTINCT lineage) AS lineages FROM st1;
SELECT carried_filled() AS filled, carried_wrong() AS wrong;
SELECT lineage = (SELECT lineage FROM sift._test_conn_gen('dim_a_col')) AS same_lineage FROM st1;

-- the parent changes after the stamp: the segment keeps the old values, the journal names the keys
UPDATE dim_a SET label = 'changed' WHERE id = 5;
UPDATE dim_a SET id = 1005 WHERE id = 6;
DELETE FROM dim_a WHERE id = 7;
INSERT INTO dim_a VALUES (999, 'late', 1);
SELECT carried_wrong() > 0 AS stale_values;
SELECT why, array_agg(key ORDER BY key) AS keys
	FROM st1 s, LATERAL sift._test_conn_changed('dim_a_col', s.lineage, s.horizon) GROUP BY why;

-- a segment written later has the new values and a later stamp; the merge of segments with different stamps fills again under one
INSERT INTO events SELECT i, 1 + i % 100, i FROM generate_series(3001, 3200) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT count(*) AS stamped_segments, count(DISTINCT horizon::text) AS horizons FROM sift._test_col_conn_stamps('events_col') WHERE lineage <> 0;
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT count(*) AS segments FROM sift.col_segments('events_col') WHERE kind = 'immutable';
CREATE TEMP TABLE st2 AS SELECT * FROM sift._test_col_conn_stamps('events_col');
SELECT count(*) AS stamps, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM st2;
SELECT (SELECT horizon::text::bigint FROM st2) > (SELECT max(horizon::text::bigint) FROM st1) AS newer_stamp;
SELECT carried_filled() AS filled, carried_wrong() AS wrong;
SELECT sift.col_verify('events_col', true);

-- the rows of a parent that is gone, whose key changed or never was have neither flag nor values (the join reads them); the others have the values of the merge
SELECT e.dim_a_id, count(*) AS n, count(f.val) AS found, min(l.val) AS label
	FROM events e JOIN sift._test_col_read('events_col', 3) f ON f.tid = e.ctid
	JOIN sift._test_col_read('events_col', 4) l ON l.tid = e.ctid
	WHERE e.dim_a_id IN (4, 5, 6, 7, 999) OR e.dim_a_id IS NULL GROUP BY 1 ORDER BY 1;

-- the answers do not change: the join is the same with and without the index
SET sift.enable_agg = on;
SELECT count(*), count(d.label), sum(d.price) FROM events e LEFT JOIN dim_a d ON d.id = e.dim_a_id;
SET sift.enable_agg = off;
SELECT count(*), count(d.label), sum(d.price) FROM events e LEFT JOIN dim_a d ON d.id = e.dim_a_id;
RESET sift.enable_agg;

-- a parent table that does not fit maintenance_work_mem is not looked up: the stamp stays empty and the leaves NULL
CREATE TABLE dim_big (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim_big SELECT i, repeat('x', 40) || i FROM generate_series(1, 40000) i;
CREATE INDEX dim_big_col ON dim_big USING sift_col (id, label);
CREATE TABLE big_events (id int PRIMARY KEY, dim_big_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO big_events SELECT i, 1 + i % 40000 FROM generate_series(1, 100) i;
CREATE INDEX big_events_col ON big_events USING sift_col (id, dim_big_id) WITH (connect = 'dim_big_id -> dim_big(id) carries label');
SET maintenance_work_mem = '1MB';
INSERT INTO big_events SELECT i, 1 + i % 40000 FROM generate_series(101, 200) i;
SELECT sift.col_promote('big_events_col') >= 0 AS promoted;
SELECT count(*) AS stamps, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM sift._test_col_conn_stamps('big_events_col');
SELECT count(val) AS filled FROM sift._test_col_read('big_events_col', 2);
RESET maintenance_work_mem;
INSERT INTO big_events SELECT i, 1 + i % 40000 FROM generate_series(201, 300) i;
SELECT sift.col_promote('big_events_col') >= 0 AS promoted;
SELECT count(*) AS stamps, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM sift._test_col_conn_stamps('big_events_col');
SELECT count(val) AS filled FROM sift._test_col_read('big_events_col', 2);
SELECT sift.col_verify('big_events_col', true);

DROP TABLE big_events, dim_big, events, dim_a CASCADE;
DROP FUNCTION carried_wrong(), carried_filled();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
