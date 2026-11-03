-- The tables of the planning matrix (bench/steps/plan_matrix.py): tenants with a skewed number of events, a dimension table (dim_a) of
-- n/100 rows spread over the tenants, a link table of key sets (links_a), and the events with a sift_col index and a BM25 index.
--
--   psql -v n=300000 -f schema.sql
--
-- Every draw is a hash of (event id, draw number), so the same n gives the same rows. Events sit in the tenant of their dim_a row
-- (dim_a.id - 1 is congruent to tenant_id - 1 modulo 40); tenant 1 has the most events, tenant 40 the fewest.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET client_min_messages = warning;
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TABLE tenants (id int PRIMARY KEY, name text NOT NULL);
INSERT INTO tenants SELECT g, 'tenant ' || g FROM generate_series(1, 40) g;

CREATE TABLE dim_a (id int PRIMARY KEY, tenant_id int NOT NULL REFERENCES tenants, label_a text NOT NULL, kind text NOT NULL);
INSERT INTO dim_a SELECT g, 1 + (g - 1) % 40, 'label ' || (g % 97), (ARRAY['alpha', 'beta', 'gamma', 'delta'])[1 + g % 4]
FROM generate_series(1, :n / 100) g;

-- 50 key sets of about a tenth of dim_a each
CREATE TABLE links_a (group_key int NOT NULL, dim_a_id int NOT NULL);
INSERT INTO links_a SELECT k, d FROM generate_series(1, 50) k, generate_series(1, :n / 100) d WHERE pg_temp.u(d, k) < 0.1;

CREATE TABLE events (
  id          bigint        PRIMARY KEY,
  tenant_id   int           NOT NULL,
  dim_a_id    int           NOT NULL,
  event_date  date          NOT NULL,
  status      smallint      NOT NULL,
  kind        text          NOT NULL,
  counter_a   int           NOT NULL,
  value_num   numeric(10,2) NOT NULL,
  title       text          NOT NULL
);
INSERT INTO events
SELECT id, t, t + 40 * floor(pg_temp.u(id, 2) * (:n / 100 / 40))::int,
       date '2024-01-01' + floor(pg_temp.u(id, 3) * 730)::int,
       floor(pg_temp.u(id, 4) * 5)::int,
       (ARRAY['click', 'view', 'buy', 'share', 'save', 'rate', 'flag', 'skip'])[1 + floor(pg_temp.u(id, 5) * 8)::int],
       floor(pg_temp.u(id, 6) * 100)::int,
       round((pg_temp.u(id, 7) * 100)::numeric, 2),
       (ARRAY['wireless', 'portable', 'compact', 'premium', 'smart', 'classic'])[1 + floor(pg_temp.u(id, 8) * 6)::int] || ' ' ||
       (ARRAY['charger', 'speaker', 'laptop', 'monitor', 'keyboard', 'tablet'])[1 + floor(pg_temp.u(id, 9) * 6)::int]
FROM (SELECT g AS id, 1 + floor(pg_temp.u(g, 1) ^ 2 * 40)::int AS t FROM generate_series(1, :n) g) x;

CREATE INDEX events_tenant ON events (tenant_id, event_date);
CREATE INDEX events_dim ON events (dim_a_id);
CREATE INDEX events_col ON events USING sift_col (tenant_id, dim_a_id, event_date, status, kind, counter_a, value_num);
CREATE INDEX events_title ON events USING sift (title);
CREATE INDEX links_a_key ON links_a (group_key, dim_a_id);
VACUUM (FREEZE, ANALYZE) events, dim_a, links_a, tenants;
