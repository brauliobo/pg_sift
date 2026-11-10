-- order_layout: the layout of the order array of each column ('whole', 'compact', 'auto', or a list of columns for each), what CREATE INDEX says about it, and that merges and REINDEX keep to it. It never adds an array.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_col_legacy_options = on;
SET sift.test_col_gorder_ends = 64;
-- plain chunks: the column format minors below tell the layouts apart (col_gorder_seek covers the seekable ones)
SET sift.test_limits = 'gord_seek_pct=0';
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
-- the whole order of the same columns, and the minor the notice names for each
CREATE INDEX w_ix ON t USING sift_col (id, ts, c) WITH (order_columns = 'id, ts, c', order_array_columns = 'ts', order_layout = 'whole');
SELECT format FROM sift.col_segments('w_ix') WHERE kind = 'immutable';
SELECT sift.col_verify('t_ix'), sift.col_verify('u_ix'), sift.col_verify('w_ix');

-- keys that tie much (a date) cost less addressed as runs: auto, which is compact a chunk at a time where that is a fifth smaller, never costs more than either layout
CREATE TABLE d AS SELECT i AS id, DATE '2026-01-01' + (i * 31 % 40) AS day FROM generate_series(1, 20000) i;
CREATE INDEX d_whole ON d USING sift_col (id, day) WITH (order_columns = 'day', order_array_columns = 'day', order_layout = 'whole');
CREATE INDEX d_compact ON d USING sift_col (id, day) WITH (order_columns = 'day', order_array_columns = 'day', order_layout = 'compact');
CREATE INDEX d_smaller ON d USING sift_col (id, day) WITH (order_columns = 'day', order_array_columns = 'day');
SELECT pg_relation_size('d_smaller') <= LEAST(pg_relation_size('d_whole'), pg_relation_size('d_compact')) AS smaller_is_least,
	   pg_relation_size('d_compact') > pg_relation_size('d_whole') AS compact_costs_more_on_ties;
SELECT sift.col_verify('d_smaller');
DROP TABLE d;
-- and compact where the keys are many and spread (a timestamp to the microsecond)
SELECT setseed(0.42);
CREATE TABLE e AS SELECT i AS id, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day' AS ts FROM generate_series(1, 20000) i;
CREATE INDEX e_smaller ON e USING sift_col (id, ts) WITH (order_columns = 'ts', order_array_columns = 'ts');
SELECT format FROM sift.col_segments('e_smaller') WHERE kind = 'immutable';
SELECT sift.col_verify('e_smaller');
DROP TABLE e;

-- a column that is both: half its rows are spread timestamps and half are 50 days, interleaved, so the chunks around a day have runs of equal keys and the others have none; auto keeps exact keys
-- in the first and compact ones in the second, one index holds both, its ordered reads are the table's and read as few blocks as the whole order (forced compact does not: it rounds every key)
SELECT setseed(0.42);
CREATE TABLE mx AS SELECT i AS id, CASE WHEN i % 2 = 0 THEN TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day' ELSE TIMESTAMP '2026-03-01' + (i % 50) * INTERVAL '1 day' END AS ts FROM generate_series(1, 60000) i;
CREATE TABLE mo AS SELECT * FROM mx;
CREATE TABLE mw AS SELECT * FROM mx;
CREATE TABLE mc AS SELECT * FROM mx;
CREATE INDEX mx_ix ON mx USING sift_col (id, ts) WITH (order_columns = 'ts', order_array_columns = 'ts');
CREATE INDEX mw_ix ON mw USING sift_col (id, ts) WITH (order_columns = 'ts', order_array_columns = 'ts', order_layout = 'whole');
CREATE INDEX mc_ix ON mc USING sift_col (id, ts) WITH (order_columns = 'ts', order_array_columns = 'ts', order_layout = 'compact');
VACUUM ANALYZE mx;
VACUUM ANALYZE mw;
VACUUM ANALYZE mc;
SELECT abs(correlation) < 0.05 AS ts_not_in_heap_order FROM pg_stats WHERE tablename = 'mx' AND attname = 'ts';
SELECT format FROM sift.col_segments('mx_ix') WHERE kind = 'immutable';
SELECT sift.col_verify('mx_ix');
-- the chunks of the middle of the order (the first and last 128 entries, twice the ends of this test, stay whole): those without a run of 8 equal keys are the compact ones, as the share in the notice above says
SELECT count(*) FILTER (WHERE maxrun < 8) AS middle_without_runs, count(*) FILTER (WHERE maxrun >= 8) AS middle_with_runs, round(100.0 * count(*) FILTER (WHERE maxrun < 8) / max(chunks), 1) AS compact_percent
FROM (SELECT seg, ch, max(n) AS maxrun, count(*) AS m, max(nv) AS nv, max(chunks) AS chunks
	  FROM (SELECT seg, pos / 512 AS ch, count(*) OVER (PARTITION BY seg, pos / 512, key) AS n, count(*) FILTER (WHERE NOT null_row) OVER (PARTITION BY seg) AS nv, (count(*) OVER (PARTITION BY seg) + 511) / 512 AS chunks
			FROM sift._test_col_gorder('mx_ix', 1)) e GROUP BY seg, ch) c
WHERE ch * 512 >= 128 AND ch * 512 + m + 128 <= nv;
CREATE FUNCTION same2(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(k::text, '','') FROM (%s) s', replace(q, '@', 'mx')) INTO a;
	EXECUTE format('SELECT string_agg(k::text, '','') FROM (%s) s', replace(q, '@', 'mo')) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
SELECT same2('SELECT ts AS k FROM @ ORDER BY ts LIMIT 50 OFFSET 700') AS asc_offset, same2('SELECT ts AS k FROM @ ORDER BY ts DESC LIMIT 50 OFFSET 15000') AS desc_offset,
	   same2('SELECT ts AS k FROM @ WHERE ts >= TIMESTAMP ''2026-03-10'' ORDER BY ts LIMIT 100 OFFSET 9000') AS range_offset, same2('SELECT ts AS k FROM @ WHERE ts = TIMESTAMP ''2026-03-05'' ORDER BY ts LIMIT 100') AS a_tie_day;
-- the blocks of a query (the buffers its plan hit or read), the second time: bounds that fall on a day of 600 equal keys
CREATE FUNCTION blocks(q text, t text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j json;
BEGIN
	q := 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || replace(q, '@', t);
	EXECUTE q INTO j;
	EXECUTE q INTO j;
	RETURN (j->0->'Plan'->>'Shared Hit Blocks')::int + (j->0->'Plan'->>'Shared Read Blocks')::int;
END $$;
CREATE TABLE mq (q text);
INSERT INTO mq VALUES ('SELECT ts FROM @ ORDER BY ts LIMIT 20 OFFSET 15000'), ('SELECT ts FROM @ ORDER BY ts DESC LIMIT 20 OFFSET 15000'),
	('SELECT ts FROM @ WHERE ts >= TIMESTAMP ''2026-03-10'' ORDER BY ts LIMIT 20 OFFSET 1000'), ('SELECT ts FROM @ WHERE ts >= TIMESTAMP ''2026-03-10'' ORDER BY ts LIMIT 20 OFFSET 9000'),
	('SELECT ts FROM @ WHERE ts <= TIMESTAMP ''2026-03-20'' ORDER BY ts DESC LIMIT 20 OFFSET 5000'),
	('SELECT ts FROM @ WHERE ts BETWEEN TIMESTAMP ''2026-03-05'' AND TIMESTAMP ''2026-03-25'' ORDER BY ts LIMIT 20 OFFSET 3000'),
	('SELECT ts FROM @ WHERE ts = TIMESTAMP ''2026-03-05'' ORDER BY ts LIMIT 100');
SELECT bool_and(blocks(q, 'mx') <= 2 * blocks(q, 'mw')) AS auto_reads_at_most_twice_the_whole, bool_or(blocks(q, 'mc') > 2 * blocks(q, 'mw')) AS compact_reads_more_than_twice FROM mq;
DROP FUNCTION same2(text), blocks(text, text);
DROP TABLE mx, mo, mw, mc, mq;

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
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
