-- The key-set dataset of the cases (bench/cases.tsv, dataset `keys`): items (the key domain), events_u (the key spread over every unit, skewed), events_w (a key
-- 50 million wide: above the byte table of a set), the key sets of 50,000 and 500,000 values, and the same sets as a membership table.
--
--   psql -v n=200000 -v dom=1000000 -f gen.sql
--
-- n is the number of events of each table, dom the number of items. Every draw is a hash of (row, draw number): the same arguments give the same rows.
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 200000
\endif
\if :{?dom}
\else
\set dom 1000000
\endif
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TABLE items AS SELECT i AS id, (i % 997) AS grp FROM generate_series(1, :dom) i;
ALTER TABLE items ADD PRIMARY KEY (id);

CREATE TABLE events_u AS
SELECT g::bigint AS id, 1 + floor(:dom * power(pg_temp.u(g, 1), 1.25))::int AS item_id,
       timestamp '2021-01-01' + (g::float8 * 1500.0 / :n) * interval '1 day' AS ts,
       (pg_temp.u(g, 2) * 29)::int2 AS kind, round((pg_temp.u(g, 3) * 10)::numeric, 1)::float8 AS score,
       CASE WHEN pg_temp.u(g, 4) < 0.015 THEN now() END AS deleted_at
FROM generate_series(1, :n) g;

CREATE TABLE events_w AS
SELECT g::bigint AS id, 1 + floor(50000000 * pg_temp.u(g, 5))::int AS wid,
       (pg_temp.u(g, 6) * 29)::int2 AS kind, round((pg_temp.u(g, 7) * 10)::numeric, 1)::float8 AS score,
       CASE WHEN pg_temp.u(g, 8) < 0.015 THEN now() END AS deleted_at
FROM generate_series(1, :n) g;

CREATE INDEX events_u_col ON events_u USING sift_col (item_id, ts, kind, score, deleted_at);
CREATE INDEX events_w_col ON events_w USING sift_col (wid, kind, score, deleted_at);
CREATE INDEX events_u_bt ON events_u (item_id, ts);
CREATE INDEX events_w_bt ON events_w (wid);

-- key sets of n items (keyset) and of n values of the wide key (keyset_w: half of them are in the table, so the list matches rows)
CREATE TABLE keyset (n int, id int);
INSERT INTO keyset SELECT 50000, id FROM items ORDER BY pg_temp.u(id, 11) LIMIT 50000;
INSERT INTO keyset SELECT 500000, id FROM items ORDER BY pg_temp.u(id, 12) LIMIT 500000;
CREATE INDEX ON keyset (n, id);
CREATE TABLE keyset_w (n int, id int);
INSERT INTO keyset_w SELECT s, wid FROM (VALUES (50000), (500000)) v(s), LATERAL (SELECT DISTINCT wid FROM events_w ORDER BY wid LIMIT v.s / 2) t;
INSERT INTO keyset_w SELECT s, 1 + floor(50000000 * pg_temp.u(i, 13))::int FROM (VALUES (50000), (500000)) v(s), LATERAL generate_series(1, s / 2) i;
CREATE INDEX ON keyset_w (n, id);
-- the same sets as a membership table with a soft-delete flag
CREATE TABLE members AS SELECT n AS group_id, id AS item_id, NULL::timestamptz AS deleted_at FROM keyset;
CREATE INDEX ON members (group_id, item_id);
CREATE INDEX ON members (item_id);
VACUUM (FREEZE, ANALYZE) items, events_u, events_w, keyset, keyset_w, members;
