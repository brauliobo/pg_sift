-- A connected index whose write buffer holds rows (inserted, updated and deleted since the last segment) while the parent changes: the rows of the found-flag plan are those of the
-- normal plan before the promotion, after it (the promotion fills the flags of the buffered rows under a new stamp), and after the parent changed again. Differential: every step
-- compares the joins with sift.enable_agg = off, whichever way the buffer keeps its rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET client_min_messages = warning;

CREATE FUNCTION found_run(q text, OUT same boolean) LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
END $$;

CREATE FUNCTION check_all() RETURNS TABLE (joined boolean, anti boolean, grouped boolean) LANGUAGE sql AS $$
	SELECT (SELECT same FROM found_run('SELECT count(*), sum(e.v) FROM ev e JOIN dim d ON d.id = e.dim_id')),
		   (SELECT same FROM found_run('SELECT count(*) FROM ev e WHERE NOT EXISTS (SELECT 1 FROM dim d WHERE d.id = e.dim_id)')),
		   (SELECT same FROM found_run('SELECT d.label, count(*), sum(e.v) FROM ev e JOIN dim d ON d.id = e.dim_id GROUP BY d.label ORDER BY d.label'))
$$;

CREATE TABLE dim (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, 'l' || i % 20 FROM generate_series(1, 300) i;
CREATE INDEX dim_col ON dim USING sift_col (id, label);
CREATE TABLE ev (id int PRIMARY KEY, dim_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 37 = 0 THEN NULL WHEN i % 41 = 0 THEN 999 ELSE 1 + i % 300 END, i % 10 FROM generate_series(1, 3000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> dim(id)');
SELECT sift.col_maintain('ev_col') IS NOT NULL AS filled;
SELECT * FROM check_all();

-- rows in the write buffer: inserted, re-keyed, deleted
INSERT INTO ev SELECT i, 1 + i % 300, i % 10 FROM generate_series(3001, 3400) i;
UPDATE ev SET dim_id = 1 + (dim_id + 7) % 300 WHERE id % 29 = 0 AND dim_id IS NOT NULL;
DELETE FROM ev WHERE id % 53 = 0;
SELECT * FROM check_all();

-- the parent changes under the buffered rows: a key inserted, one deleted, one re-keyed, a label changed
INSERT INTO dim VALUES (999, 'new');
DELETE FROM dim WHERE id = 5;
UPDATE dim SET id = 1000 WHERE id = 6;
UPDATE dim SET label = 'changed' WHERE id <= 10;
SELECT * FROM check_all();

-- the promotion fills the buffered rows under a new stamp
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
SELECT * FROM check_all();
SELECT fill FROM sift.col_connections WHERE child = 'ev_col'::regclass;

-- the parent changes again, and more rows are buffered, with the probe and with the scan as the way the fill knows the keys
SET sift.test_conn_fill = 2;
DELETE FROM dim WHERE id = 7;
INSERT INTO ev SELECT i, 1 + i % 300, i % 10 FROM generate_series(3401, 3600) i;
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
SELECT * FROM check_all();
SET sift.test_conn_fill = 1;
UPDATE dim SET id = 1001 WHERE id = 8;
INSERT INTO ev SELECT i, 1 + i % 300, i % 10 FROM generate_series(3601, 3800) i;
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
SELECT * FROM check_all();
SELECT sift.col_maintain('ev_col') IS NOT NULL AS maintained;
SELECT * FROM check_all();
RESET sift.test_conn_fill;

DROP FUNCTION check_all();
DROP TABLE ev, dim;
DROP FUNCTION found_run(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
