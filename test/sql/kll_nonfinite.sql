-- The quantile of a column is over its finite values: NaN, an infinite float and an infinite date or timestamp are no values, in the sketches of an index and in the exact answer alike.
-- A numeric column that is not a scaled int64 (no precision, more than 18 digits) has no sketch: the index is refused, as the part that held its first rows would crash the backend.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;

CREATE TABLE readings (id int8 PRIMARY KEY, value_num float8, d date, ts timestamp, n numeric(12, 3));
INSERT INTO readings SELECT i, i::float8, DATE '2020-01-01' + i, TIMESTAMP '2020-01-01' + i * interval '1 minute', i / 10.0 FROM generate_series(1, 2000) i;
INSERT INTO readings VALUES (2001, 'Infinity', 'infinity', 'infinity', 1), (2002, '-Infinity', '-infinity', '-infinity', 1), (2003, 'NaN', NULL, NULL, 1);
CREATE INDEX readings_col ON readings USING sift_col (id, value_num, d, ts, n) WITH (sketches = 'kll(value_num, d, ts, n)');

SELECT c.col, sift.approx_quantiles('readings_col', c.col, ARRAY[0, 1]) AS sketch, sift.approx_quantiles('readings_col', c.col, ARRAY[0, 1], 'true') AS exact
FROM (VALUES ('value_num'), ('d'), ('ts'), ('n')) c(col) ORDER BY 1;
SELECT sift.approx_percentile('readings', 'value_num', 1) AS table_max, sift.approx_percentile('readings', 'value_num', 0) AS table_min;

\set ON_ERROR_STOP off
\set VERBOSITY terse
CREATE TABLE ledger (id int8, amount numeric, wide numeric(30, 4), exact numeric(12, 2));
CREATE INDEX ledger_amount ON ledger USING sift_col (id, amount) WITH (sketches = 'kll(amount)');
CREATE INDEX ledger_wide ON ledger USING sift_col (id, wide) WITH (sketches = 'kll(wide)');
CREATE INDEX ledger_exact ON ledger USING sift_col (id, exact) WITH (sketches = 'kll(exact)');
\set ON_ERROR_STOP on
-- a part flushes its rows (and writes their sketches) at about 40000 rows
INSERT INTO ledger SELECT i, i, i, i FROM generate_series(1, 45000) i;
INSERT INTO ledger VALUES (45001, 'Infinity', 1, 1), (45002, 'NaN', 1, 1);
SELECT sift.approx_percentile('ledger', 'amount', 1) AS exact_max, sift.approx_percentile('ledger_exact', 'exact', 1) AS sketch_max;

DROP TABLE readings, ledger;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
