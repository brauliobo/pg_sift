-- ALTER INDEX ... SET of a sift_col option that names columns is checked against the columns of the index, as CREATE INDEX does, and a value that does
-- not fit fails the ALTER and leaves the options as they were (it must never leave an index that fails the queries, inserts and VACUUM of its table).
-- Every ALTER runs in a new session (\c): the first statement of a session that has not used sift yet loads the library while it runs, which used to be too
-- late for the check.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;

CREATE TABLE ao (id int, pid int, d date, ts timestamptz, cc text, f float8, metrics jsonb, parts jsonb) WITH (autovacuum_enabled = off);
INSERT INTO ao SELECT i, i % 50, date '2024-01-01' + i % 300, timestamptz '2024-01-01' + i * interval '1 minute', 'c' || i % 7, i % 10,
	jsonb_build_object('a', i % 10), jsonb_build_array(jsonb_build_object('kind', 't' || i % 3))
FROM generate_series(1, 3000) i;
CREATE INDEX ao_paths ON ao USING sift_col (id, pid, d, ts, cc, f, metrics, parts) WITH (paths = 'metrics.* float8, parts[]{kind text}');

CREATE TABLE ap (id int, pid int, d date, ts timestamptz, cc text, f float8) WITH (autovacuum_enabled = off);
INSERT INTO ap SELECT i, i % 50, date '2024-01-01' + i % 300, timestamptz '2024-01-01' + i * interval '1 minute', 'c' || i % 7, i % 10 FROM generate_series(1, 3000) i;
CREATE INDEX ap_plain ON ap USING sift_col (id, pid, d, ts, cc, f);

-- a table with more default dimensions than the cube holds leaves no room for an explicit list of calendar columns
CREATE TABLE aw (c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int, c9 int, c10 int, c11 int, c12 int, c13 int, c14 int, c15 int, c16 int, d1 date, d2 date) WITH (autovacuum_enabled = off);
INSERT INTO aw SELECT i, i % 3, i % 4, i % 5, i % 6, i % 7, i % 8, i % 9, i % 3, i % 4, i % 5, i % 6, i % 7, i % 8, i % 9, i % 2, date '2024-01-01' + i % 30, date '2024-01-01' + i % 20 FROM generate_series(1, 2000) i;
CREATE INDEX aw_wide ON aw USING sift_col (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, d1, d2);

-- an index with paths: calendar cells need a schema without them
\c
ALTER INDEX ao_paths SET (calendar_columns = 'd');
\c
ALTER INDEX ao_paths SET (calendar_columns = 'all');
-- a column that is not in the index, and one of a type the option does not take
\c
ALTER INDEX ao_paths SET (set_columns = 'nope');
\c
ALTER INDEX ao_paths SET (set_columns = 'f');
\c
ALTER INDEX ao_paths SET (key_presence = 'nope');
\c
ALTER INDEX ao_paths SET (key_presence = 'cc');
\c
ALTER INDEX ao_paths SET (rollup_dims = 'nope, pid');
\c
ALTER INDEX ao_paths SET (rollup_dims = 'f, pid');
\c
ALTER INDEX ao_paths SET (order_columns = 'nope');
\c
ALTER INDEX ao_paths SET (order_columns = 'cc');
\c
ALTER INDEX ao_paths SET (return_columns = 'nope');
\c
ALTER INDEX ao_paths SET (return_columns = 'pid');
\c
ALTER INDEX ap_plain SET (calendar_columns = 'id');
\c
ALTER INDEX ap_plain SET (calendar_columns = 'nope');
\c
ALTER INDEX ap_plain SET (calendar_columns = 'd, d');
\c
ALTER INDEX aw_wide SET (calendar_columns = 'd1, d2');

-- nothing was changed, and the table still takes statements
\c
SELECT relname, reloptions FROM pg_class WHERE relname IN ('ao_paths', 'ap_plain', 'aw_wide') ORDER BY 1;
SET client_min_messages = warning;
INSERT INTO ao VALUES (0, 0, '2024-01-01', now(), 'x', 0, '{"a": 1}', '[]');
SET sift.enable_agg = on;
SELECT count(*) FROM ao;
VACUUM ao;

-- values that fit are applied, and the table goes on working
\c
ALTER INDEX ao_paths SET (set_columns = 'pid', key_presence = 'pid', order_columns = 'pid', rollup_dims = 'pid, d', return_columns = 'cc');
\c
ALTER INDEX ap_plain SET (calendar_columns = 'd, ts');
SET client_min_messages = warning;
INSERT INTO ao VALUES (-1, 0, '2024-01-01', now(), 'x', 0, '{"a": 1}', '[]');
INSERT INTO ap VALUES (-1, 0, '2024-01-01', now(), 'x', 0);
SELECT count(*) FROM ao;
SELECT count(*) FROM ap;
VACUUM ao;
VACUUM ap;
SELECT sift.col_verify('ao_paths', true), sift.col_verify('ap_plain', true);
