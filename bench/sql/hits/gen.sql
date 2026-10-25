-- The ClickBench-like dataset of the cases (bench/cases.tsv, dataset `hits`): a wide web-analytics table of page views (user, region, time, url, referrer, search phrase, ads,
-- screen), generated here with skewed users, regions and phrases and a mostly empty search phrase, as ClickBench's `hits` is. It is not ClickBench's data: the numbers are not
-- ClickBench results (that is `bench/bench analytics clickbench`, which downloads the real parquet files).
--
--   psql -v n=1000000 -f gen.sql
--
-- n is the number of rows. Every draw is a hash of (row, draw number): the same arguments give the same rows. Like ClickBench's PostgreSQL tables, only the primary key is a btree.
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 1000000
\endif
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TABLE hits AS
SELECT g::bigint AS watch_id,
       timestamp '2013-07-01' + (g::float8 / :n) * 31 * interval '1 day' AS event_time,
       (timestamp '2013-07-01' + (g::float8 / :n) * 31 * interval '1 day')::date AS event_date,
       1 + floor(power(pg_temp.u(g, 1), 2) * 3000)::int AS counter_id,
       (hashint8extended(g, 2) & 2147483647)::int AS client_ip,
       1 + floor(power(pg_temp.u(g, 3), 3) * 9000)::int AS region_id,
       1 + floor(greatest(:n / 10, 100) * power(pg_temp.u(g, 4), 2.2))::bigint AS user_id,
       (pg_temp.u(g, 5) * 30)::int2 AS os,
       (pg_temp.u(g, 6) * 20)::int2 AS user_agent,
       'http://' || (ARRAY['example.com','google.com','news.example','shop.example','video.example','forum.example'])[1 + floor(power(pg_temp.u(g, 7), 2) * 6)::int]
         || '/' || floor(pg_temp.u(g, 8) * 100000)::int AS url,
       CASE WHEN pg_temp.u(g, 9) < 0.5 THEN '' ELSE 'http://' || (ARRAY['google.com','example.com','news.example'])[1 + floor(pg_temp.u(g, 10) * 3)::int] || '/r' || floor(pg_temp.u(g, 11) * 1000)::int END AS referer,
       (pg_temp.u(g, 12) < 0.1)::int::int2 AS is_refresh,
       (ARRAY[1024, 1280, 1366, 1440, 1600, 1920, 2560])[1 + floor(pg_temp.u(g, 13) * 7)::int]::int2 AS resolution_width,
       CASE WHEN pg_temp.u(g, 14) < 0.9 THEN '' ELSE 'model' || floor(pg_temp.u(g, 15) * 200)::int END AS mobile_phone_model,
       CASE WHEN pg_temp.u(g, 16) < 0.9 THEN '' ELSE 'phrase ' || floor(power(pg_temp.u(g, 17), 3) * 50000)::int END AS search_phrase,
       CASE WHEN pg_temp.u(g, 18) < 0.9 THEN 0 ELSE 1 + floor(pg_temp.u(g, 19) * 20)::int END::int2 AS adv_engine_id
FROM generate_series(1, :n) g;

ALTER TABLE hits ADD PRIMARY KEY (watch_id);
CREATE INDEX hits_col ON hits USING sift_col (event_date, event_time, counter_id, client_ip, region_id, user_id, os, user_agent, url, referer, is_refresh, resolution_width, mobile_phone_model, search_phrase, adv_engine_id);
VACUUM (FREEZE, ANALYZE) hits;
