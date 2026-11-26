-- The shared fixtures of the bytes-per-row gate (scripts/bpr/run.py): every draw is a hash of the row number, so the same :rows give the same rows.
-- 16,384 rows are two granules of 8,192: enough to amortise the fixed header of a part to under 0.5 B/row, small enough to load in under a second.
--
--   psql -v rows=16384 -f fixtures.sql
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 16384
\endif
DROP TABLE IF EXISTS bpr_base_wide, bpr_base_narrow, bpr_base_json, bpr_base_text, bpr_pairs;

CREATE TABLE bpr_base_wide AS
SELECT i::int8 AS id, i % 64 AS grp, timestamptz '2024-01-01' + (i % 86400) * interval '1 second' AS ts, i * 3 AS v1,
	(hashint4(i) & 1023)::int8 AS v2, ((hashint4(i + 1) & 65535) / 100.0)::numeric(10,2) AS amount, (hashint4(i + 2) & 4095) * 0.25 AS score,
	'cat_' || i % 16 AS cat, 'txt_' || (hashint4(i + 3) & 255) AS txt, jsonb_build_object('k', i % 8, 'flag', i % 3 = 0) AS doc,
	'w' || i % 50 || ' w' || (hashint4(i + 4) & 127) AS body
FROM generate_series(1, :rows) i;

CREATE TABLE bpr_base_narrow AS
SELECT i AS id, i % 64 AS grp, i::int8 AS v1, (i * 3)::int8 AS v2, i % 1000 AS v3, repeat('x', 40) AS pad
FROM generate_series(1, :rows) i;

CREATE TABLE bpr_base_json AS
SELECT i AS id, jsonb_build_object('channel', 'ch_' || i % 6, 'score', i % 10, 'items', jsonb_build_array(i % 4, (i + 1) % 4)) AS doc,
	ARRAY[i % 16, i % 64, (hashint4(i) & 255)] AS tags
FROM generate_series(1, :rows) i;

CREATE TABLE bpr_base_text AS
SELECT i AS id, i % 32 AS grp,
	CASE WHEN i % 5 < 4 THEN 'wire ' ELSE '' END || CASE WHEN i % 8 = 0 THEN 'switch ' ELSE '' END || CASE WHEN i % 97 = 0 THEN 'quantum ' ELSE '' END
		|| 'tail' || (hashint4(i * 23) & 2047) || ' tail' || (hashint4(i * 29) & 2047) AS body
FROM generate_series(1, :rows) i;

CREATE TABLE bpr_pairs AS SELECT i AS a, i % 64 AS b FROM generate_series(1, :rows / 8) i;
ANALYZE;
