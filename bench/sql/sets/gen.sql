-- The set-filter dataset (bench/steps/sets.sh): a fact table filtered by a set of entity ids, and the membership table that names the sets.
--
--   psql -v nf=3000000 -v ne=200000 -f gen.sql
--
-- Every draw is a hash of (row id, draw number), so the same sizes give the same rows on every run and on every PostgreSQL plan.
-- entities  ne ids; the facts per entity follow a zipf law (exponent 0.85) over a random permutation of the ids: at 3,000,000 facts and
--           200,000 entities p50 5, p90 20, p99 135 and a largest entity of about 85,000 facts
-- facts     nf rows in the order of id (the order of arrival: 85% of a day's facts arrive within 15 days, the rest up to 600 days late): entity_id,
--           30 sources (zipf 1.1), 40 languages (zipf 1.5), 3 sentiments, a float8 score, a date from 2021-01-01 on (more facts towards the end),
--           1.5% soft-deleted
-- memberships  sets of entity ids drawn uniformly at random, sets overlap freely: 60 of 100, 20 of 1,000, 4 of 2,000, 4 of 5,000, 8 of 10,000, 2 of 100,000 (334,000 rows)
\set ON_ERROR_STOP on
SET work_mem = '256MB';
SET max_parallel_workers_per_gather = 0;
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
-- cumulative zipf weights of ranks 1..n and their total
CREATE FUNCTION pg_temp.zipf_cum(n int, s float8) RETURNS float8[] LANGUAGE sql IMMUTABLE AS
  $$ SELECT array_agg(c ORDER BY r) FROM (SELECT r, sum(power(r::float8, -s)) OVER (ORDER BY r) - power(r::float8, -s) AS c FROM generate_series(1, n) r) t $$;
CREATE FUNCTION pg_temp.zipf_tot(n int, s float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS
  $$ SELECT sum(power(r::float8, -s)) FROM generate_series(1, n) r $$;

DROP TABLE IF EXISTS facts, memberships, entities, facts_src CASCADE;
CREATE TABLE entities AS SELECT i AS id FROM generate_series(1, :ne) i;
ALTER TABLE entities ADD PRIMARY KEY (id);

-- rank r of the permutation carries the weight r^-0.85; cum is the start of its slice of [0, total)
CREATE TEMP TABLE slices AS
SELECT id, sum(w) OVER (ORDER BY r) - w AS cum
FROM (SELECT id, row_number() OVER (ORDER BY hashint8extended(id, 7)) AS r, power((row_number() OVER (ORDER BY hashint8extended(id, 7)))::float8, -0.85) AS w FROM entities) t;
CREATE INDEX ON slices (cum);
CREATE TEMP TABLE slice_total AS SELECT sum(power(r::float8, -0.85)) AS tot FROM generate_series(1, :ne) r;
ANALYZE slices, slice_total;

CREATE TEMP TABLE facts_src AS
SELECT row_number() OVER (ORDER BY created_at, rnd) AS id, entity_id, source_id, lang_id, sentiment, fact_date, score, deleted_at
FROM (
  SELECT s.id AS entity_id,
         width_bucket(pg_temp.u(g.f, 2) * pg_temp.zipf_tot(30, 1.1), pg_temp.zipf_cum(30, 1.1))::int2 AS source_id,
         width_bucket(pg_temp.u(g.f, 3) * pg_temp.zipf_tot(40, 1.5), pg_temp.zipf_cum(40, 1.5))::int2 AS lang_id,
         (CASE WHEN pg_temp.u(g.f, 4) < .15 THEN 0 WHEN pg_temp.u(g.f, 5) < .18 THEN 1 ELSE 2 END)::int2 AS sentiment,
         g.d AS fact_date,
         round(least(10, greatest(0, 8 + 1.8 * (pg_temp.u(g.f, 6) + pg_temp.u(g.f, 8) + pg_temp.u(g.f, 9) - 1.5) * 1.6))::numeric, 1)::float8 AS score,
         CASE WHEN pg_temp.u(g.f, 10) < 0.015 THEN timestamptz '2026-10-01' - pg_temp.u(g.f, 11) * interval '300 days' END AS deleted_at,
         g.d::timestamp + ((CASE WHEN pg_temp.u(g.f, 12) < .85 THEN floor(pg_temp.u(g.f, 13) * 15) ELSE 15 + floor(pg_temp.u(g.f, 13) * 585) END) + pg_temp.u(g.f, 14)) * interval '1 day' AS created_at,
         pg_temp.u(g.f, 15) AS rnd
  FROM (SELECT f, date '2021-01-01' + floor(2100 * sqrt(pg_temp.u(f, 1)))::int AS d, pg_temp.u(f, 16) * (SELECT tot FROM slice_total) AS v
        FROM generate_series(1::bigint, :nf) f) g
  CROSS JOIN LATERAL (SELECT id FROM slices WHERE cum <= g.v ORDER BY cum DESC LIMIT 1) s
) q;

CREATE TABLE facts AS SELECT id::bigint AS id, entity_id, source_id, lang_id, sentiment, fact_date, score, deleted_at FROM facts_src ORDER BY id;

CREATE TABLE memberships (group_id int, entity_id int, deleted_at timestamptz);
DO $$
DECLARE g int := 0; sz int; cnt int; n int;
BEGIN
  FOREACH sz IN ARRAY ARRAY[100, 1000, 2000, 5000, 10000, 100000] LOOP
    cnt := CASE sz WHEN 100 THEN 60 WHEN 1000 THEN 20 WHEN 2000 THEN 4 WHEN 5000 THEN 4 WHEN 10000 THEN 8 ELSE 2 END;
    FOR n IN 1..cnt LOOP
      g := g + 1;
      INSERT INTO memberships SELECT g, id, NULL FROM entities ORDER BY hashint8extended(id, 1000 + g) LIMIT sz;
    END LOOP;
  END LOOP;
END $$;
CREATE INDEX ON memberships (group_id, entity_id);
CREATE INDEX ON memberships (entity_id);
ANALYZE entities, facts, memberships;
