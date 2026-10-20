-- One fact table with a key column of every type an array parameter can filter, for bench/sets/types.py (about 200000 distinct keys per column).
--   createdb types && psql -d types -f bench/sets/types.sql
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET max_parallel_workers_per_gather = 0;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id int PRIMARY KEY, k2 int2, k4 int4, k8 int8, kt text, kd date, kts timestamp, ktz timestamptz, amount numeric(10, 2), g int);
INSERT INTO t
SELECT i, (i::bigint * 7919) % 30000 - 15000, 1 + (i::bigint * 7919) % 200000, 5000000000 + (i::bigint * 7919) % 200000,
       'key-' || ((i::bigint * 7919) % 200000), date '1990-01-01' + (((i::bigint * 7919) % 200000) / 10)::int,
       timestamp '2000-01-01' + ((i::bigint * 7919) % 200000) * interval '1 minute',
       timestamptz '2000-01-01' + ((i::bigint * 7919) % 200000) * interval '1 minute', i % 100 / 10.0, i % 7
FROM generate_series(1, 2000000) i;
CREATE INDEX t_col ON t USING sift_col (k2, k4, k8, kt, kd, kts, ktz, amount, g);
CREATE INDEX t_k2 ON t (k2);
CREATE INDEX t_k4 ON t (k4);
CREATE INDEX t_k8 ON t (k8);
CREATE INDEX t_kt ON t (kt);
CREATE INDEX t_kd ON t (kd);
CREATE INDEX t_kts ON t (kts);
CREATE INDEX t_ktz ON t (ktz);
VACUUM (FREEZE, ANALYZE) t;
