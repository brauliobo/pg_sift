-- The tables of the join matrix (bench/steps/join_matrix.py): a fact, dimensions of three sizes, a 1:1 extension, an M:N link, a
-- partitioned fact and a self reference. Every draw is a hash of (row, draw number), so the same n gives the same rows.
--
--   psql -v n=1000000 -f schema.sql
--
--   f        fact, n rows: tenant_id (skewed, 40), lookup_id (skewed, 8), keys of every type to the dimensions, a parent_id into f
--   ds       small dimension, max(100, n/1000) rows: int id, text code, uuid uid, composite (tenant_id, id), nullable reference nul_id in f
--   dl       large dimension, n/2 rows: bigint id, a reference ds_id to ds (snowflake)
--   cal      calendar of 731 days: a date key
--   fx       1:1 extension of f (95% of its ids)
--   link     M:N between f and dl (n rows)
--   fp       f's rows with id % 4 = 0, range-partitioned by event_date in 8 quarters
--   key_sets  20 sets of 1,000 keys of dl (set_id, dl_id): the scope sets of semi and anti joins
-- 5% of the fact's references point past the end of the dimension (orphans: LEFT, anti and NOT IN have rows to find); 10% of nul_id is NULL.
-- Dimension columns share names: g_low (about 20 values), g_high (n_dim/10 values), v (0..9999), pct (0..99 uniform), attr (a..d).
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 4;
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
CREATE FUNCTION pg_temp.tenant(int) RETURNS int LANGUAGE sql IMMUTABLE
  AS $$ SELECT 1 + floor(pg_temp.u($1, 1) ^ 2 * 40)::int $$;
SELECT greatest(100, :n / 1000) AS ns, :n / 2 AS nl \gset

CREATE TABLE tenants (id int PRIMARY KEY, name text NOT NULL);
INSERT INTO tenants SELECT g, 'tenant ' || g FROM generate_series(1, 40) g;

CREATE TABLE ds (id int PRIMARY KEY, tenant_id int NOT NULL, code text NOT NULL, uid uuid NOT NULL, g_low smallint NOT NULL,
                 g_high int NOT NULL, v int NOT NULL, pct smallint NOT NULL, attr text NOT NULL);
INSERT INTO ds SELECT g, pg_temp.tenant(g), 'S' || lpad(g::text, 7, '0'), md5('s' || g)::uuid, floor(pg_temp.u(g, 2) * 20),
       floor(pg_temp.u(g, 3) * (:ns / 10)), floor(pg_temp.u(g, 4) * 10000), floor(pg_temp.u(g, 5) * 100),
       (ARRAY['a', 'b', 'c', 'd'])[1 + floor(pg_temp.u(g, 6) * 4)::int] FROM generate_series(1, :ns) g;

CREATE TABLE dl (id bigint PRIMARY KEY, ds_id int NOT NULL, g_low smallint NOT NULL, g_high int NOT NULL, v int NOT NULL,
                 pct smallint NOT NULL, attr text NOT NULL);
INSERT INTO dl SELECT g, 1 + floor(pg_temp.u(g, 1) * :ns)::int, floor(pg_temp.u(g, 2) * 20), floor(pg_temp.u(g, 3) * (:nl / 10)),
       floor(pg_temp.u(g, 4) * 10000), floor(pg_temp.u(g, 5) * 100), (ARRAY['a', 'b', 'c', 'd'])[1 + floor(pg_temp.u(g, 6) * 4)::int]
FROM generate_series(1, :nl) g;

CREATE TABLE cal (d date PRIMARY KEY, g_low smallint NOT NULL, g_high int NOT NULL, v int NOT NULL, pct smallint NOT NULL, attr text NOT NULL);
INSERT INTO cal SELECT d::date, extract(dow FROM d), extract(doy FROM d) + 366 * (extract(year FROM d) - 2024), (d::date - date '2024-01-01'),
       floor(pg_temp.u((d::date - date '2024-01-01')::bigint, 5) * 100), (ARRAY['a', 'b', 'c', 'd'])[1 + extract(month FROM d)::int % 4]
FROM generate_series(date '2024-01-01', date '2025-12-31', interval '1 day') d;

CREATE TABLE f (
  id         bigint        PRIMARY KEY,
  tenant_id  int           NOT NULL,
  lookup_id  smallint      NOT NULL,
  ds_id      int           NOT NULL,
  dl_id      bigint        NOT NULL,
  nul_id     int,
  ds_code    text          NOT NULL,
  ds_uid     uuid          NOT NULL,
  parent_id  bigint,
  event_date date          NOT NULL,
  status     smallint      NOT NULL,
  kind       text          NOT NULL,
  qty        int           NOT NULL,
  val        numeric(8,2)  NOT NULL,
  pct        smallint      NOT NULL
);
INSERT INTO f
SELECT id, pg_temp.tenant(s), 1 + floor(pg_temp.u(id, 2) ^ 3 * 8)::int, s, 1 + floor(pg_temp.u(id, 3) * :nl * 1.05)::bigint,
       CASE WHEN pg_temp.u(id, 4) < 0.1 THEN NULL ELSE s END, 'S' || lpad(s::text, 7, '0'), md5('s' || s)::uuid,
       CASE WHEN id = 1 OR pg_temp.u(id, 5) < 0.05 THEN NULL ELSE 1 + floor(pg_temp.u(id, 6) * (id - 1))::bigint END,
       date '2024-01-01' + floor(pg_temp.u(id, 7) * 731)::int, floor(pg_temp.u(id, 8) * 5),
       (ARRAY['click', 'view', 'buy', 'share', 'save', 'rate', 'flag', 'skip'])[1 + floor(pg_temp.u(id, 9) * 8)::int],
       floor(pg_temp.u(id, 10) * 10000), round((pg_temp.u(id, 11) * 100)::numeric, 2), floor(pg_temp.u(id, 12) * 100)
FROM (SELECT g AS id, 1 + floor(pg_temp.u(g, 1) ^ 2 * :ns * 1.05)::int AS s FROM generate_series(1, :n) g) x;

CREATE TABLE fx (id bigint PRIMARY KEY, g_low smallint NOT NULL, g_high int NOT NULL, v int NOT NULL, pct smallint NOT NULL, attr text NOT NULL);
INSERT INTO fx SELECT g, floor(pg_temp.u(g, 22) * 20), floor(pg_temp.u(g, 23) * (:n / 10)), floor(pg_temp.u(g, 24) * 10000),
       floor(pg_temp.u(g, 25) * 100), (ARRAY['a', 'b', 'c', 'd'])[1 + floor(pg_temp.u(g, 26) * 4)::int]
FROM generate_series(1, :n) g WHERE pg_temp.u(g, 21) >= 0.05;

CREATE TABLE link (f_id bigint NOT NULL, dl_id bigint NOT NULL);
INSERT INTO link SELECT 1 + floor(pg_temp.u(g, 31) * :n)::bigint, 1 + floor(pg_temp.u(g, 32) * :nl)::bigint FROM generate_series(1, :n) g;

CREATE TABLE key_sets (set_id int NOT NULL, dl_id bigint NOT NULL);
INSERT INTO key_sets SELECT DISTINCT s, 1 + floor(pg_temp.u(s * 1000000 + m, 41) * :nl)::bigint FROM generate_series(1, 20) s, generate_series(1, 1000) m;
CREATE UNIQUE INDEX key_sets_set ON key_sets (set_id, dl_id);

CREATE TABLE fp (LIKE f) PARTITION BY RANGE (event_date);
SELECT format('CREATE TABLE fp_%s PARTITION OF fp FOR VALUES FROM (%L) TO (%L)', q, d, d + interval '3 months')
FROM (SELECT row_number() OVER () AS q, d::date AS d FROM generate_series(date '2024-01-01', date '2025-10-01', interval '3 months') d) x \gexec
INSERT INTO fp SELECT * FROM f WHERE id % 4 = 0;

-- the b-tree baseline: every join key and the scope filters
CREATE INDEX f_ds ON f (ds_id);
CREATE INDEX f_dl ON f (dl_id);
CREATE INDEX f_nul ON f (nul_id);
CREATE INDEX f_parent ON f (parent_id);
CREATE INDEX f_code ON f (ds_code);
CREATE INDEX f_uid ON f (ds_uid);
CREATE INDEX f_date ON f (event_date);
CREATE INDEX f_tenant ON f (tenant_id, lookup_id);
CREATE UNIQUE INDEX ds_code ON ds (code);
CREATE UNIQUE INDEX ds_uid ON ds (uid);
CREATE UNIQUE INDEX ds_tenant_id ON ds (tenant_id, id);
CREATE INDEX dl_ds ON dl (ds_id);
CREATE INDEX link_f ON link (f_id);
CREATE INDEX link_dl ON link (dl_id, f_id);
CREATE INDEX fp_ds ON fp (ds_id);
CREATE INDEX fp_date ON fp (event_date);
CREATE INDEX fp_tenant ON fp (tenant_id, lookup_id);
VACUUM (FREEZE, ANALYZE) f, ds, dl, cal, fx, link, fp, tenants, key_sets;
