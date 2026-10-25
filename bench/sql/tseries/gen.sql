-- The time-series dataset of the cases (bench/cases.tsv, dataset `tseries`): readings of 1,000 devices (ten metrics each) appended in time order over 90 days, as a monitoring
-- table is: ts, device, metric, value. Like most such tables it has a btree on (device, ts) and one on ts.
--
--   psql -v n=1000000 -f gen.sql
--
-- n is the number of rows. Every draw is a hash of (row, draw number): the same arguments give the same rows.
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 1000000
\endif
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TABLE metrics AS
SELECT timestamp '2025-01-01' + (g::float8 / :n) * 90 * interval '1 day' AS ts,
       1 + floor(pg_temp.u(g, 1) * 1000)::int AS device,
       floor(pg_temp.u(g, 2) * 10)::int2 AS metric,
       round((50 + 20 * (pg_temp.u(g, 3) + pg_temp.u(g, 4) + pg_temp.u(g, 5) - 1.5))::numeric, 3)::float8 AS value
FROM generate_series(1, :n) g;
CREATE INDEX metrics_ts ON metrics (ts);
CREATE INDEX metrics_device_ts ON metrics (device, ts);
CREATE INDEX metrics_col ON metrics USING sift_col (ts, device, metric, value);
VACUUM (FREEZE, ANALYZE) metrics;
