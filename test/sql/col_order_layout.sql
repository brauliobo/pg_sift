-- order_layout: the layout of the order array of each column ('whole', 'compact', 'auto', or a list of columns for each), what CREATE INDEX says about it, and that merges and REINDEX keep to it. It never adds an array.
CREATE EXTENSION pg_sift;
SET sift.test_col_gorder_ends = 64;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE t (id int, ts timestamp, c date, name text);
INSERT INTO t SELECT i, TIMESTAMP '2026-01-01' + (i * 7919 % 20011) * INTERVAL '1 minute', DATE '2026-01-01' + (i * 31 % 300), 'n' || i FROM generate_series(1, 20000) i;
CREATE TABLE o AS SELECT * FROM t;

-- the value is checked when it is set
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'sequence');
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'dense');
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'compact ts');
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'compact:');
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'compact: nope');
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'compact: name');
CREATE INDEX x ON t USING sift_col (id, ts) WITH (order_layout = 'compact: ts; whole: ts');

-- the layout of each segment and the bytes of each column, as CREATE INDEX says them
CREATE INDEX t_ix ON t USING sift_col (id, ts, c) WITH (order_columns = 'id, ts, c', order_array_columns = 'id, ts, c', order_layout = 'compact: ts, c; whole: id');
SELECT format, rows FROM sift.col_segments('t_ix') WHERE kind = 'immutable';
-- the layout names every column, the array is only on ts: the option adds none
CREATE INDEX u_ix ON t USING sift_col (id, ts, c) WITH (order_columns = 'id, ts, c', order_array_columns = 'ts', order_layout = 'compact');
SELECT format FROM sift.col_segments('u_ix') WHERE kind = 'immutable';
SELECT sift.col_verify('t_ix'), sift.col_verify('u_ix');

-- ordered results are the table's, with a write buffer
INSERT INTO t SELECT 20000 + i, TIMESTAMP '2027-01-01' + i * INTERVAL '1 minute', DATE '2026-06-01', 'm' || i FROM generate_series(1, 300) i;
INSERT INTO o SELECT 20000 + i, TIMESTAMP '2027-01-01' + i * INTERVAL '1 minute', DATE '2026-06-01', 'm' || i FROM generate_series(1, 300) i;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(k::text, '','') FROM (%s) s', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(k::text, '','') FROM (%s) s', replace(q, '@', 'o')) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
SELECT same('SELECT ts AS k FROM @ ORDER BY ts LIMIT 50 OFFSET 700') AS ts_asc,
	   same('SELECT ts AS k FROM @ ORDER BY ts DESC LIMIT 50 OFFSET 5000') AS ts_desc,
	   same('SELECT id AS k FROM @ WHERE id >= 12000 ORDER BY id LIMIT 50 OFFSET 3000') AS id_range,
	   same('SELECT c AS k FROM @ WHERE c <= DATE ''2026-03-01'' ORDER BY c DESC LIMIT 100 OFFSET 20') AS c_range;

-- a merge and a REINDEX write the layout of the option as it is then; a setting that is wrong is refused
ALTER INDEX t_ix SET (order_layout = 'bogus');
ALTER INDEX t_ix SET (order_layout = 'whole');
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
SELECT sift.col_merge('t_ix', true) >= 0 AS merged;
SELECT format, rows FROM sift.col_segments('t_ix') WHERE kind = 'immutable';
SELECT same('SELECT ts AS k FROM @ ORDER BY ts LIMIT 50 OFFSET 700') AS ts_asc, same('SELECT c AS k FROM @ ORDER BY c DESC LIMIT 50 OFFSET 9000') AS c_desc;
ALTER INDEX t_ix SET (order_layout = 'compact: ts');
REINDEX INDEX t_ix;
SELECT format, rows FROM sift.col_segments('t_ix') WHERE kind = 'immutable';
SELECT same('SELECT ts AS k FROM @ ORDER BY ts DESC LIMIT 50 OFFSET 700') AS ts_desc, same('SELECT id AS k FROM @ ORDER BY id LIMIT 50 OFFSET 9000') AS id_asc;
SELECT sift.col_verify('t_ix');

DROP FUNCTION same(text);
DROP TABLE o;
DROP TABLE t;
DROP EXTENSION pg_sift;
