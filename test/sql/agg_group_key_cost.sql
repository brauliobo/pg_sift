-- Two prices of a grouped scan that were off, each measured against the normal plan on 200,000 rows:
--  - a filter that leaves a handful of rows to a unit (`counter_id = 62` and a month: 1 row in 800): the dictionary of the group key is mapped for those rows
--    only, not for every entry of the unit (a filter that keeps most rows maps them all, agg_key_entries_cost);
--  - a key that is an expression of an integer column with as many values as rows: each row evaluates the expression and looks up an id in a table of 200,000
--    (the credit for the expressions the normal plan prices at one operator assumed a numeric program, and no id was priced for an integer column).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
CREATE TABLE gk_hits WITH (autovacuum_enabled = false) AS
SELECT g::bigint AS id, (timestamp '2013-07-01' + (g::float8 / 200000) * 31 * interval '1 day')::date AS day,
       1 + floor(power(pg_temp.u(g, 1), 2) * 3000)::int AS counter_id, (hashint8extended(g, 2) & 2147483647)::int AS ip, (pg_temp.u(g, 12) < 0.1)::int AS refresh,
       'http://' || (ARRAY['a.example', 'b.example', 'c.example', 'd.example', 'e.example', 'f.example'])[1 + floor(power(pg_temp.u(g, 7), 2) * 6)::int] || '/' || floor(pg_temp.u(g, 8) * 100000)::int AS url
FROM generate_series(1, 200000) g;
SET client_min_messages = warning;
CREATE INDEX gk_hits_col ON gk_hits USING sift_col (id, day, counter_id, ip, refresh, url);
RESET client_min_messages;
VACUUM ANALYZE gk_hits;

SELECT q, used FROM (VALUES
	(1, $$SELECT url, count(*) AS c FROM gk_hits WHERE counter_id = 62 AND day >= '2013-07-01' AND day <= '2013-07-31' AND refresh = 0 GROUP BY url ORDER BY c DESC LIMIT 10$$),
	(2, $$SELECT ip, ip - 1, ip - 2, ip - 3, count(*) AS c FROM gk_hits GROUP BY ip, ip - 1, ip - 2, ip - 3 ORDER BY c DESC LIMIT 10$$),
	(3, $$SELECT counter_id, count(*) AS c FROM gk_hits WHERE refresh = 0 GROUP BY counter_id ORDER BY c DESC LIMIT 10$$)
) v(q, sql), LATERAL (SELECT bool_or(used) AS used FROM sift.explain_why(v.sql)) w ORDER BY q;

DROP TABLE gk_hits;
DROP EXTENSION pg_sift;
