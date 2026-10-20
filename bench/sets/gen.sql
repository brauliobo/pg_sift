
-- Synthetic data of the set filter benchmarks (docs/SET-FILTERS.md): entities (a dimension table with a city, country, category, rating class,
-- chain and region), a fact table of events (zipf events per entity, dates with a backfill tail, 1.5% soft-deleted) and group_members
-- (a membership table: 60 groups of 100, 20 of 1,000, 8 of 10,000 and 2 of 100,000 entities).
--   psql -v np=200000 -v nr=3000000 -v sch=sets -f gen.sql      deterministic (setseed); the sizes of the published runs
\set ON_ERROR_STOP on
DROP SCHEMA IF EXISTS :sch CASCADE; CREATE SCHEMA :sch; SET search_path = :sch, public;
SELECT setseed(0.32);


-- zipf helper: draws an index in 1..n with weight rank^-s
CREATE OR REPLACE FUNCTION pg_temp.zipf_cum(n int, s float8) RETURNS float8[] LANGUAGE sql IMMUTABLE AS
$$ SELECT array_agg(c ORDER BY r) FROM (SELECT r, sum(power(r::float8, -s)) OVER (ORDER BY r) - power(r::float8, -s) AS c FROM generate_series(1, n) r) t $$;
CREATE OR REPLACE FUNCTION pg_temp.zipf_tot(n int, s float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS
$$ SELECT sum(power(r::float8, -s)) FROM generate_series(1, n) r $$;

CREATE TABLE countries AS SELECT i AS id, 'C' || i AS name FROM generate_series(1, 60) i;
-- 3000 cities, each in one country; the country share is zipf
CREATE TABLE cities AS
SELECT i AS id, width_bucket(random() * pg_temp.zipf_tot(60, 1.0), pg_temp.zipf_cum(60, 1.0)) AS country_id, 'city' || i AS name
FROM generate_series(1, 3000) i;

CREATE TABLE entities AS
SELECT p AS id,
       c.id AS city_id, c.country_id,
       width_bucket(random() * pg_temp.zipf_tot(12, 1.2), pg_temp.zipf_cum(12, 1.2)) AS category_id,
       least(5, greatest(0, round(random() * 2 + 2.2 + (random() - 0.5) * 1.5)::int)) AS tier,
       CASE WHEN random() < 0.55 THEN NULL ELSE width_bucket(random() * pg_temp.zipf_tot(300, 1.0), pg_temp.zipf_cum(300, 1.0)) END AS chain_id,
       (c.country_id * 7 + c.id % 7) AS region_id
FROM (SELECT p, width_bucket(random() * pg_temp.zipf_tot(3000, 0.9), pg_temp.zipf_cum(3000, 0.9)) AS cid FROM generate_series(1, :np) p) x
JOIN cities c ON c.id = x.cid;
ALTER TABLE entities ADD PRIMARY KEY (id);

-- events per entity: zipf over a random permutation of the entities
CREATE TEMP TABLE pw AS
SELECT id, power((row_number() OVER (ORDER BY random()))::float8, -0.85) AS w FROM entities;
CREATE TEMP TABLE pc AS SELECT id, sum(w) OVER (ORDER BY id) - w AS cum FROM pw;
CREATE INDEX ON pc (cum);
CREATE TEMP TABLE ptot AS SELECT sum(w) AS tot FROM pw;
ANALYZE pc;

CREATE TABLE events_src AS
SELECT row_number() OVER (ORDER BY created_at, rnd) AS id, entity_id, channel_id, lang_id, sentiment, event_date, score, deleted_at
FROM (
  SELECT pcl.id AS entity_id,
         width_bucket(random() * pg_temp.zipf_tot(30, 1.1), pg_temp.zipf_cum(30, 1.1))::int2 AS channel_id,
         width_bucket(random() * pg_temp.zipf_tot(40, 1.5), pg_temp.zipf_cum(40, 1.5))::int2 AS lang_id,
         (CASE WHEN random() < .15 THEN 0 WHEN random() < .18 THEN 1 ELSE 2 END)::int2 AS sentiment,
         d AS event_date,
         round(least(10, greatest(0, 8 + 1.8 * (random() + random() + random() - 1.5) * 1.6))::numeric, 1)::float8 AS score,
         CASE WHEN random() < 0.015 THEN now() - random() * interval '300 days' END AS deleted_at,
         d::timestamp + ((CASE WHEN random() < .85 THEN floor(random() * 15) ELSE 15 + floor(random() * 585) END) + random()) * interval '1 day' AS created_at,
         random() AS rnd
  FROM (SELECT date '2021-01-01' + floor(2100 * sqrt(random()))::int AS d, random() * (SELECT tot FROM ptot) AS u FROM generate_series(1, :nr)) g
  CROSS JOIN LATERAL (SELECT id FROM pc WHERE cum <= g.u ORDER BY cum DESC LIMIT 1) pcl
) q;
ALTER TABLE events_src ALTER COLUMN id TYPE bigint;

-- groups: sizes 100 (60 groups), 1000 (20), 10000 (8), 100000 (2), then 2000 (4) and 5000 (4); members drawn uniformly at random, groups overlap freely
CREATE TABLE group_members (group_id int, entity_id int, deleted_at timestamptz);
DO $$
DECLARE g int := 0; sz int; cnt int; n int;
BEGIN
  FOREACH sz IN ARRAY ARRAY[100, 1000, 10000, 100000, 2000, 5000] LOOP
    cnt := CASE sz WHEN 100 THEN 60 WHEN 1000 THEN 20 WHEN 10000 THEN 8 WHEN 100000 THEN 2 ELSE 4 END;
    FOR n IN 1..cnt LOOP
      g := g + 1;
      EXECUTE format('INSERT INTO %I SELECT %s, id, NULL FROM %I ORDER BY random() LIMIT %s', 'group_members', g, 'entities', sz);
    END LOOP;
  END LOOP;
END $$;
CREATE INDEX ON group_members (group_id, entity_id);
CREATE INDEX ON group_members (entity_id);
ANALYZE events_src, entities, cities, countries, group_members;
