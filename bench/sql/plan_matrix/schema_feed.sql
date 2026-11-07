-- The tables of the order-array planning matrix (bench/steps/plan_matrix.py --schema feed): a feed of events with a timestamp, an id, a low-cardinality column and the
-- conditions of a list page (soft delete, a flag, a status at several pass rates), read through a global order of ts and id (order_array_columns, order_layout auto).
--
--   psql -v n=300000 -v wbuf=5000 -f schema_feed.sql        wbuf: rows inserted after the index is built, which stay in its write buffer (0: none)
--
-- Every draw is a hash of (row id, draw number), so the same n gives the same rows. qNN is 1 for NN% of the rows (q001: 0.01%), the others 0.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET client_min_messages = warning;
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TABLE feed (
  id          bigint        PRIMARY KEY,
  ts          timestamptz   NOT NULL,
  kind        smallint      NOT NULL,
  active      boolean       NOT NULL,
  deleted_at  timestamptz,
  q99         smallint      NOT NULL,
  q50         smallint      NOT NULL,
  q10         smallint      NOT NULL,
  q1          smallint      NOT NULL,
  q001        smallint      NOT NULL,
  title       text          NOT NULL
);
CREATE FUNCTION pg_temp.fill(lo bigint, hi bigint) RETURNS void LANGUAGE sql AS $$
  INSERT INTO feed
  SELECT g, timestamptz '2024-01-01' + interval '1 second' * floor(pg_temp.u(g, 1) * 63072000)::int, floor(pg_temp.u(g, 2) * 8)::int,
         pg_temp.u(g, 3) < 0.95, CASE WHEN pg_temp.u(g, 4) < 0.02 THEN timestamptz '2025-01-01' END,
         (pg_temp.u(g, 5) < 0.99)::int, (pg_temp.u(g, 6) < 0.5)::int, (pg_temp.u(g, 7) < 0.1)::int, (pg_temp.u(g, 8) < 0.01)::int, (pg_temp.u(g, 9) < 0.0001)::int,
         'item ' || g
  FROM generate_series(lo, hi) g
$$;
SELECT pg_temp.fill(1, :n);
CREATE INDEX feed_ts ON feed (ts);
CREATE INDEX feed_col ON feed USING sift_col (id, ts, kind, active, deleted_at, q99, q50, q10, q1, q001)
  WITH (order_columns = 'id, ts', order_array_columns = 'id, ts', order_layout = 'auto', write_buffer_limit = 100000);
VACUUM (FREEZE, ANALYZE) feed;
SELECT pg_temp.fill(:n + 1, :n + :wbuf) WHERE :wbuf > 0;
ANALYZE feed;

-- the latest row of each entity (a sub-select that is the first row of its entity in the order of ts): a hundred rows an entity, ts unique, the order array of ts grouped by the entity
-- (order_array_group), a b-tree on (entity, ts) as the twin; status is 7 for one row in 400
CREATE TABLE dim_a (id int PRIMARY KEY, tier smallint NOT NULL);
INSERT INTO dim_a SELECT g, g % 10 FROM generate_series(1, greatest(:n / 100, 1)) g;
CREATE TABLE pings (id bigint PRIMARY KEY, entity int NOT NULL, ts timestamptz NOT NULL, status smallint NOT NULL);
CREATE FUNCTION pg_temp.fill_pings(lo bigint, hi bigint, ents int) RETURNS void LANGUAGE sql AS $$
  INSERT INTO pings SELECT g, 1 + floor(pg_temp.u(g, 11) * ents)::int, timestamptz '2024-01-01' + interval '1 second' * (g % 100003) * 600 + interval '1 microsecond' * g, (g % 400)::smallint
  FROM generate_series(lo, hi) g
$$;
SELECT pg_temp.fill_pings(1, :n, greatest(:n / 100, 1));
CREATE INDEX pings_ent_ts ON pings (entity, ts);
CREATE INDEX pings_col ON pings USING sift_col (id, entity, ts, status) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'entity');
VACUUM (FREEZE, ANALYZE) dim_a, pings;
SELECT pg_temp.fill_pings(:n + 1, :n + :wbuf, greatest(:n / 100, 1)) WHERE :wbuf > 0;
ANALYZE pings;
