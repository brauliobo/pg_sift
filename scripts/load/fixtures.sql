-- The shared rows of the load gate (scripts/load/run.py): every value is a hash of the row number, so the same :rows give the same rows on every run and host.
-- 8,192 rows are one granule: enough for every engine to keep parts, small enough that a case builds its tables in milliseconds.
--
--   psql -v rows=8192 -f fixtures.sql
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 8192
\endif
DROP TABLE IF EXISTS load_base_fact;
CREATE TABLE load_base_fact AS
SELECT i::int8 AS id, i % 64 AS grp, (hashint4(i) & 1023)::int8 AS v,
	CASE WHEN i % 5 < 4 THEN 'wire ' ELSE '' END || CASE WHEN i % 8 = 0 THEN 'switch ' ELSE '' END || 'tail' || (hashint4(i * 23) & 2047) AS body
FROM generate_series(1, :rows) i;
ALTER TABLE load_base_fact ADD PRIMARY KEY (id);
ANALYZE load_base_fact;
