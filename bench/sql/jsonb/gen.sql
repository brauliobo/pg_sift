-- synthetic documents for the jsonb benchmarks: items[]{kind,sub,flag,score}, scores.* (dynamic keys), attrs{channel,nested.a.b,tags[],mixed}
-- psql -v n=100000 -v strs=0 -f gen.sql   (strs: share of numbers written as strings in the numeric paths: 0 is a clean table; 0.05 adds the strings, the mixed-type case)
\if :{?n} \else \set n 100000 \endif
\if :{?strs} \else \set strs 0 \endif
\set ON_ERROR_STOP on
SET work_mem = '256MB';
CREATE OR REPLACE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
DROP TABLE IF EXISTS docs_src;
CREATE UNLOGGED TABLE docs_src AS
SELECT g.id,
       1 + floor(pg_temp.u(g.id, 1) * 200)::int AS grp,
       date '2023-01-01' + floor(pg_temp.u(g.id, 2) * 1000)::int AS day,
       jsonb_build_object(
         'channel', (ARRAY['web','app','api','mail','shop'])[1 + floor(pg_temp.u(g.id, 3) * 5)::int],
         'nested', jsonb_build_object('a', jsonb_build_object('b', floor(pg_temp.u(g.id, 4) * 100)::int)),
         'tags', (SELECT coalesce(jsonb_agg('t' || floor(pg_temp.u(g.id * 10 + k, 5) * 30)::int), '[]') FROM generate_series(1, floor(pg_temp.u(g.id, 6) * 4)::int) k),
         'mixed', CASE WHEN pg_temp.u(g.id, 7) < 0.7 THEN to_jsonb(floor(pg_temp.u(g.id, 8) * 10)::int) WHEN pg_temp.u(g.id, 7) < 0.9 THEN to_jsonb('n/a'::text) ELSE 'null'::jsonb END
       ) AS attrs,
       (SELECT coalesce(jsonb_object_agg('k' || (1 + floor(pg_temp.u(g.id * 100 + k, 9) * 20)::int),
                 CASE WHEN pg_temp.u(g.id * 100 + k, 10) < 0.9 THEN to_jsonb(round((pg_temp.u(g.id * 100 + k, 11) * 10)::numeric, 1)) WHEN pg_temp.u(g.id * 100 + k, 10) < 0.9 + :strs THEN to_jsonb('x'::text) ELSE 'null'::jsonb END), '{}')
          FROM generate_series(1, 2 + floor(pg_temp.u(g.id, 12) * 6)::int) k) AS metrics_a,
       (SELECT coalesce(jsonb_agg(jsonb_build_object(
                 'kind', 'c' || (1 + floor(pg_temp.u(g.id * 100 + k, 13) * 12)::int),
                 'sub', 's' || (1 + floor(pg_temp.u(g.id * 100 + k, 14) * 40)::int),
                 'flag', (ARRAY['fa','fb','fc'])[1 + floor(pg_temp.u(g.id * 100 + k, 15) * 3)::int],
                 'score', CASE WHEN pg_temp.u(g.id * 100 + k, 16) < 0.9 THEN to_jsonb(round((pg_temp.u(g.id * 100 + k, 17) * 10)::numeric, 1))
                               WHEN pg_temp.u(g.id * 100 + k, 16) < 0.9 + :strs THEN to_jsonb('7.5'::text) ELSE 'null'::jsonb END)
                 ORDER BY k) FILTER (WHERE pg_temp.u(g.id * 100 + k, 18) < 0.97), '[]')
          FROM generate_series(1, floor(pg_temp.u(g.id, 19) * 9)::int) k) AS items
FROM generate_series(1, :n) AS g(id);

DROP TABLE IF EXISTS docs;
CREATE TABLE docs AS SELECT * FROM docs_src;
ALTER TABLE docs ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS jmap;
CREATE TABLE jmap AS SELECT 'c' || k AS kind, 's' || s AS sub, 'L' || ((k + s) % 4) AS label FROM generate_series(1, 12) k, generate_series(1, 40) s;
ANALYZE docs;
