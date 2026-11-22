-- The grouping of an aggregate scan. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_group_estimates
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- The groups the planner expects of a calendar key on a date column come from the days the column takes (the index's range, no more than the days the
-- conditions on it leave), not from PostgreSQL's count of distinct dates: a month, a year or a week of a date is a few groups, not as many as the dates.
-- Each statement is planned as a SiftAgg scan; its estimated rows must be within a factor of two of the groups it returns.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ge_rows (id int PRIMARY KEY, dim_a_id int NOT NULL, day date NOT NULL, val int NOT NULL) WITH (autovacuum_enabled = false);
-- 700 days, a thousand rows a day on average
INSERT INTO ge_rows SELECT i, 1 + i % 50, date '2025-01-01' + (hashint4(i) & 1023) % 700, i % 10 FROM generate_series(1, 100000) i;
CREATE INDEX ge_rows_col ON ge_rows USING sift_col (id, dim_a_id, day, val);
VACUUM (FREEZE, ANALYZE) ge_rows;

CREATE FUNCTION est_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN substring(l FROM 'rows=(\d+)')::bigint; END IF;
	END LOOP;
	RETURN NULL;
END $$;

CREATE FUNCTION groups_of(q text) RETURNS TABLE (planned_rows_ok bool, groups bigint) LANGUAGE plpgsql AS $$
DECLARE e bigint;
BEGIN
	e := est_rows(q);
	EXECUTE format('SELECT count(*) FROM (%s) x', q) INTO groups;
	planned_rows_ok := e IS NOT NULL AND e BETWEEN groups / 2.0 AND groups * 2.0;
	RETURN NEXT;
END $$;

SELECT k AS key, (groups_of(format('SELECT %s, count(*) FROM ge_rows GROUP BY 1', k))).*
FROM (VALUES ('date_trunc(''year'', day)'), ('date_trunc(''quarter'', day)'), ('date_trunc(''month'', day)'), ('date_trunc(''week'', day)'),
	('extract(year FROM day)'), ('extract(month FROM day)'), ('extract(isodow FROM day)'), ('date_trunc(''month'', day)::date')) v(k);

-- the conditions on the date bound the days: a quarter holds three months and about thirteen weeks
SELECT k AS key, (groups_of(format('SELECT %s, count(*) FROM ge_rows WHERE day >= %L AND day < %L GROUP BY 1', k, '2025-04-01', '2025-07-01'))).*
FROM (VALUES ('date_trunc(''month'', day)'), ('date_trunc(''week'', day)'), ('extract(month FROM day)')) v(k);

-- a column of timestamps that was never analyzed has no histogram: the index's smallest and largest value give the span of the buckets
CREATE TABLE ge_ts (id int PRIMARY KEY, ts timestamptz NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ge_ts SELECT i, timestamptz '2025-01-01' + (hashint4(i) & 1023) % 700 * interval '1 day' + i % 86400 * interval '1 second' FROM generate_series(1, 100000) i;
CREATE INDEX ge_ts_col ON ge_ts USING sift_col (id, ts);
VACUUM (FREEZE) ge_ts;
SELECT k AS key, (groups_of(format('SELECT %s, count(*) FROM ge_ts GROUP BY 1', k))).*
FROM (VALUES ('date_trunc(''month'', ts AT TIME ZONE ''UTC'')'), ('date_trunc(''week'', ts AT TIME ZONE ''UTC'')')) v(k);

DROP FUNCTION groups_of(text), est_rows(text);
DROP TABLE ge_rows, ge_ts;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_group_estimates

-- >>> agg_group_key_cost
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- Two prices of a grouped scan that were off, each measured against the normal plan on 200,000 rows:
--  - a filter that leaves a handful of rows to a unit (`counter_id = 62` and a month: 1 row in 800): the dictionary of the group key is mapped for those rows
--    only, not for every entry of the unit (a filter that keeps most rows maps them all, agg_key_entries_cost);
--  - a key that is an expression of an integer column with as many values as rows: each row evaluates the expression and looks up an id in a table of 200,000
--    (the credit for the expressions the normal plan prices at one operator assumed a numeric program, and no id was priced for an integer column).
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
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_group_key_cost

-- >>> agg_group_key_lookup
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- A table filtered to a few rows and joined to a grouped sub-select on the key of its groups wants those groups only. The sub-select asks for them: its keys are looked up in
-- the filtered table first (a semi join), where the groups of the whole table cost a scan or a full index pass. The join above stays as written. The plan of the two forms
-- is set by cost; the rows must be the same as with the scan off, for keys that are NULL, that repeat in the filtered table and that have no row in it, with no worker and
-- with three.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tenants (id int PRIMARY KEY, name text NOT NULL, set_id int);
CREATE UNIQUE INDEX tenants_name ON tenants (name);
INSERT INTO tenants SELECT g, 'tenant_' || lpad(g::text, 5, '0'), CASE WHEN g % 10 = 0 THEN NULL ELSE 1 + g % 5 END FROM generate_series(1, 4000) g;
-- a table whose key repeats: the filtered rows of a key come twice
CREATE TABLE marks (tenant_id int, label text);
INSERT INTO marks SELECT 1 + (g * 13) % 4000, CASE WHEN g % 2 = 0 THEN 'even' ELSE 'odd' END FROM generate_series(1, 6000) g;
CREATE INDEX marks_label ON marks (label);
-- link_id: NULL for a tenth of the rows and 1 to 400 for the rest
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, category_id int NOT NULL, value_num numeric(10,2), link_id int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + (g * 7919) % 4000, 1 + g % 19, CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 500) / 5.0 END, CASE WHEN g % 10 = 0 THEN NULL ELSE 1 + g % 400 END
       FROM generate_series(1, 60000) g;
-- tenants 3901 to 4000 have no event
DELETE FROM events WHERE tenant_id > 3900;
CREATE INDEX events_tenant ON events (tenant_id, category_id);
CREATE INDEX events_link ON events (link_id);
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, category_id, value_num, link_id) WITH (key_cut = tenant_id);
VACUUM (FREEZE, ANALYZE) events, tenants, marks;

CREATE TABLE lookup_q (n int, label text, q text);
INSERT INTO lookup_q VALUES
	(1, 'one tenant by its name',            'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id WHERE t.name = ''tenant_01234'''),
	(2, 'a tenant without events',           'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id WHERE t.name = ''tenant_03950'''),
	(3, 'a range of names',                  'SELECT t.name, a.n, a.s FROM (SELECT tenant_id, count(*) AS n, sum(value_num) AS s FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id WHERE t.name BETWEEN ''tenant_00100'' AND ''tenant_00130'' ORDER BY 1'),
	(4, 'a fifth of the tenants',            'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id WHERE t.set_id = 3 ORDER BY 1'),
	(5, 'a key that repeats in the table',   'SELECT m.label, a.tenant_id, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN marks m ON m.tenant_id = a.tenant_id WHERE m.label = ''odd'' AND m.tenant_id < 40 ORDER BY 2, 1'),
	(6, 'NULL keys, equal groups',           'SELECT t.name, a.n FROM (SELECT link_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.link_id WHERE t.name < ''tenant_00005'' ORDER BY 1'),
	(7, 'a key that is an expression',       'SELECT t.name, a.n FROM (SELECT tenant_id + 0 AS k, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.k WHERE t.name = ''tenant_02500'''),
	(8, 'two tables filtered',               'SELECT t.name, a.n, b.s FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN (SELECT tenant_id, sum(value_num) AS s FROM events GROUP BY 1) b ON b.tenant_id = a.tenant_id JOIN tenants t ON t.id = a.tenant_id WHERE t.name = ''tenant_00777'''),
	(9, 'a having and an order with a limit', 'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1 HAVING count(*) > 10) a JOIN tenants t ON t.id = a.tenant_id WHERE t.set_id = 2 ORDER BY a.n DESC, t.name LIMIT 5'),
	(10, 'a left join keeps the groups',     'SELECT t.name, a.n FROM tenants t LEFT JOIN (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a ON t.id = a.tenant_id WHERE t.name = ''tenant_03950'''),
	(11, 'the table on the other side',      'SELECT t.name, a.n FROM tenants t JOIN (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a ON a.tenant_id = t.id WHERE t.name IN (''tenant_00042'', ''tenant_03999'', ''tenant_00043'')  ORDER BY 1');
-- the plan of each statement: the keys looked up first (the table events is read by its indexes, no scan), the scan (SiftAgg), or the normal plan; the rows of the two equal
CREATE FUNCTION lookup_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scans int := 0; probes int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scans := scans + (l LIKE '%Custom Scan (SiftAgg)%')::int;
		probes := probes + (l LIKE '%Index%events_tenant%' AND l LIKE '%Cond%' OR l LIKE '%Index%Scan%events_tenant%')::int;
	END LOOP;
	RETURN CASE WHEN scans > 0 THEN 'scan x' || scans WHEN probes > 0 THEN 'index lookups' ELSE 'normal' END;
END $$;
CREATE FUNCTION lookup_rows(q text, scan bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers_reset(q, scan, workers) $$;
SELECT n, label, lookup_plan(q) AS plan, lookup_rows(q, false, 0) <> '' AS has_rows, lookup_rows(q, true, 0) = lookup_rows(q, false, 0) AS same, lookup_rows(q, true, 3) = lookup_rows(q, false, 0) AS same_workers FROM lookup_q ORDER BY n;

DROP FUNCTION lookup_plan(text);
DROP FUNCTION lookup_rows(text, bool, int);
DROP TABLE lookup_q, events, marks, tenants;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_group_key_lookup

-- >>> agg_group_key_width
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- SQL keys and the grouping-set discriminator share the mask's full key space.
SET max_parallel_workers_per_gather=0;
CREATE TABLE events(id int,a int,b int,c int,d int,e int,f int,g int,h int,payload text);
INSERT INTO events SELECT i,i%3,i%2,i%3,i%2,i%3,i%2,i%3,
 CASE WHEN i%11=0 THEN NULL ELSE i%2 END,repeat('payload',50) FROM generate_series(1,100000)i;
CREATE INDEX events_col ON events USING sift_col(id,a,b,c,d,e,f,g,h);
VACUUM(FREEZE,ANALYZE)events;
CREATE FUNCTION check_query(q text)RETURNS TABLE(pushed bool,differences bigint)LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
 SET LOCAL sift.enable_agg=off;
 EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS '||q;
 SET LOCAL sift.enable_agg=on;
 EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS '||q;
 SELECT count(*)INTO differences FROM((TABLE r_off EXCEPT ALL TABLE r_on)UNION ALL(TABLE r_on EXCEPT ALL TABLE r_off))d;
 pushed:=false;
 FOR line IN EXECUTE 'EXPLAIN(COSTS OFF) '||q LOOP pushed:=pushed OR line LIKE '%Custom Scan (SiftAgg)%';END LOOP;
 DROP TABLE r_off,r_on;
 RETURN NEXT;
END$$;
-- Nine physical keys: eight ordinary SQL columns and the set number.
-- A repeated set preserves duplicate rows; GROUPING distinguishes a real NULL key.
SELECT * FROM check_query($q$
 SELECT a,b,c,d,e,f,g,h,GROUPING(a,h) AS mask,count(*) AS n,count(DISTINCT id) AS ids
 FROM events GROUP BY GROUPING SETS((a,b,c,d,e,f,g,h),(a,b,c,d,e,f,g,h),(a,h),())
$q$);
-- Many keys must start in hash mode instead of overflowing a product of direct capacities.
-- Repeated expressions with different types remain distinct SQL group keys.
SELECT * FROM check_query($q$
 SELECT a AS a1, a::bigint AS a2, a::text AS a3, b AS b1, b::bigint AS b2, b::text AS b3,
 c AS c1, c::bigint AS c2, c::text AS c3, d AS d1, d::bigint AS d2, d::text AS d3,
 e AS e1, e::bigint AS e2, e::text AS e3, f AS f1, f::bigint AS f2, f::text AS f3,
 g AS g1, g::bigint AS g2, g::text AS g3, h AS h1, h::bigint AS h2, h::text AS h3,count(*) AS n
 FROM events GROUP BY 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24
$q$);
DROP FUNCTION check_query(text);
DROP TABLE events;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_group_key_width

-- >>> agg_group_leaf_lossy
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- The group leaf a set-returning function reads (the jsonb array behind jsonb_array_elements, the object behind jsonb_each) is found by its attribute, its path and
-- its kind alone, with none of the tests a scalar leaf gets (not lossy, no '#' in its name): a group leaf is never lossy and has no name, and the virtual leaves
-- (the presence leaf '<col>#present', the members read as expressions) are scalars, which the kind keeps out. This pins it: an index with a lossy scalar leaf (char
-- without a length), the presence leaf and both group kinds gives the rows of the normal plan.
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE gl (id int PRIMARY KEY, g int, c bpchar, doc jsonb, m jsonb) WITH (autovacuum_enabled = false);
INSERT INTO gl SELECT i, i % 7, CASE i % 4 WHEN 0 THEN 'a' WHEN 1 THEN 'a  ' WHEN 2 THEN 'b' ELSE NULL END,
	CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN '{}'
		 ELSE jsonb_build_object('items', jsonb_build_array(jsonb_build_object('tag', 't' || i % 5, 'n', i % 9), jsonb_build_object('tag', 't' || (i + 1) % 5, 'n', NULL),
															 jsonb_build_object('n', i % 3))) END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE jsonb_build_object('x', i % 5 + 0.5, 'y', i % 3 + 0.25) END
	FROM generate_series(1, 2000) i;
SET client_min_messages = warning;
CREATE INDEX gl_col ON gl USING sift_col (id, g, c, doc, m) WITH (paths = 'doc.items[]{tag text, n int4}, m.* float8');
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) gl;

CREATE FUNCTION rows_of(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION uses_sift(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE shapes (q text PRIMARY KEY, sql text);
INSERT INTO shapes VALUES
	('elements by tag', $q$SELECT e->>'tag' AS tag, count(*) AS n, sum((e->>'n')::int) AS s FROM gl, jsonb_array_elements(doc->'items') e GROUP BY 1$q$),
	('elements by the lossy column', $q$SELECT c, count(*) AS n, sum((e->>'n')::int) AS s FROM gl, jsonb_array_elements(doc->'items') e GROUP BY 1$q$),
	('elements of a filtered row set', $q$SELECT g, count(*) AS n FROM gl, jsonb_array_elements(doc->'items') e WHERE c = 'a' AND e->>'tag' = 't1' GROUP BY 1$q$),
	('exists over the elements', $q$SELECT g, count(*) AS n FROM gl WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(doc->'items') e WHERE e->>'tag' = 't2') GROUP BY 1$q$),
	('document is null beside the elements', $q$SELECT count(*) FILTER (WHERE doc IS NULL) AS nulls, count(*) FILTER (WHERE doc = '{}') AS empty, count(*) AS n FROM gl$q$),
	('each of the keys', $q$SELECT key, count(*) AS n, sum(value::float8) AS s FROM gl, jsonb_each(m) GROUP BY 1$q$),
	('each by the lossy column', $q$SELECT c, count(*) AS n FROM gl, jsonb_each(m) GROUP BY 1$q$),
	('each of a filtered row set', $q$SELECT key, count(*) AS n FROM gl, jsonb_each(m) WHERE c IS NOT NULL AND g < 4 GROUP BY 1$q$),
	('elements of rows with a column', $q$SELECT g, count(*) AS n FROM gl, jsonb_array_elements(doc->'items') e WHERE c IS NOT NULL GROUP BY 1$q$);

SELECT q, uses_sift(sql) AS on_index, rows_of(sql, true) = rows_of(sql, false) AS same_rows FROM shapes ORDER BY q;

DROP TABLE shapes;
DROP FUNCTION rows_of(text, bool), uses_sift(text);
DROP TABLE gl;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_group_leaf_lossy

-- >>> agg_group_memory
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
-- The group table of a scan is in memory, and a hash aggregate gets work_mem x hash_mem_multiplier. When the groups the planner expects
-- (with the bytes of a group) do not fit, the scan is charged the spill of its partial states, as the normal plan is charged its own, and
-- the dearer of the two is not used: the refusal is `group_memory`, and only when the spill decided it (`cost` otherwise).
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '30s';

CREATE TABLE facts (id int, u int, v int, w float8);
INSERT INTO facts SELECT i, i, i % 100, i * 0.5 FROM generate_series(1, 30000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, u, v, w);
VACUUM (FREEZE, ANALYZE) facts;

CREATE FUNCTION q(keys text) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT %s count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi, avg(v) AS a2, sum(w) AS s2, min(w) AS lo2, max(v) AS hi2 FROM facts %s', coalesce(keys || ',', ''), CASE WHEN keys IS NULL THEN '' ELSE 'GROUP BY ' || keys END) $$;

-- the plan of a query and why it was or was not the scan, for a hash memory and a number of groups
CREATE FUNCTION why(keys text, mem text, mult text DEFAULT '1') RETURNS TABLE (hash_kb int, groups text, used boolean, slug text, detail boolean)
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('work_mem', mem, true), set_config('hash_mem_multiplier', mult, true);
	RETURN QUERY SELECT (pg_size_bytes(current_setting('work_mem')) / 1024 * current_setting('hash_mem_multiplier')::float8)::int,
		coalesce(keys, 'none'), e.used, e.slug, e.reason LIKE '%groups need % kB, a hash aggregate gets % kB%'
	FROM sift.explain_why(q(keys)) e;
END $$;

-- 30,000 groups of about 200 bytes fit the 8 MB of the defaults; they do not fit 512 kB, and the sort of the normal plan is the cheaper spill
SELECT * FROM why('u', '4MB', '2');
SELECT * FROM why('u', '1MB');
-- 100 groups, or one, fit any memory
SELECT * FROM why('v', '64kB');
SELECT * FROM why(NULL, '64kB');

-- the same rows, whichever plan the memory picks
SET sift.enable_agg = off;
CREATE TEMP TABLE want AS SELECT u, count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts GROUP BY u;
RESET sift.enable_agg;
CREATE TEMP TABLE got_scan AS SELECT u, count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts GROUP BY u;
SET work_mem = '1MB';
SET hash_mem_multiplier = 1;
CREATE TEMP TABLE got_normal AS SELECT u, count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts GROUP BY u;
RESET work_mem;
RESET hash_mem_multiplier;
SELECT (SELECT count(*) FROM (TABLE want EXCEPT TABLE got_scan) d) AS missing_scan,
       (SELECT count(*) FROM (TABLE got_scan EXCEPT TABLE want) d) AS extra_scan,
       (SELECT count(*) FROM (TABLE want EXCEPT TABLE got_normal) d) AS missing_normal,
       (SELECT count(*) FROM want) AS groups;

DROP FUNCTION why(text, text, text), q(text);
DROP TABLE facts;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_group_memory

-- >>> agg_group_open_cost
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
-- A scan of one table that groups starts at about 0.3 ms (the source, its first units), which was priced at 0.7: a tenant of 6,700 rows of 100,000 by month took the
-- normal plan, 1.8 ms (an index-only scan and a hash aggregate), where the scan takes 0.85. The scan is now priced from 0.14, so the tenants whose rows the scan reads
-- faster than the index does (6,700 rows: 1.8 against 0.85 ms; 15,800: 4.0 against 0.83) are the scan's, and a tenant of 1,300 rows (0.43 against 0.59 ms) stays the normal plan's.
-- The prices (scripts/plan-margin.sh): tenant 1 scan 248.8 against normal 545.4, 2 196.0 against 236.2 (17%), 10 169.3 against 94.3, 40 156.6 against 55.2; the tenants between
-- 3 and 4 are a tie of the prices (1% and 14%) and are not asserted.
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, event_date date NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + floor(power((hashint4(g) & 16777215) / 16777216.0, 2) * 40)::int, date '2024-01-01' + (hashint4(g + 1) & 16777215) % 730, (ARRAY['a', 'b', 'c', 'd'])[1 + (hashint4(g + 2) & 3)]
FROM generate_series(1, 100000) g;
CREATE INDEX events_tenant ON events (tenant_id, event_date);
CREATE INDEX events_col ON events USING sift_col (tenant_id, event_date, kind);
VACUUM (FREEZE, ANALYZE) events;

CREATE FUNCTION month_plan(tenant int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text;
	q text = format($q$SELECT date_trunc('month', event_date) AS m, count(*) FROM events WHERE tenant_id = %s GROUP BY 1$q$, tenant);
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'SiftAgg' THEN RETURN 'scan'; END IF;
	END LOOP;
	RETURN 'normal';
END $$;

SELECT t AS tenant, (SELECT count(*) FROM events WHERE tenant_id = t) AS rows, month_plan(t) AS plan FROM unnest(ARRAY[1, 2, 10, 40]) t ORDER BY t;

DROP FUNCTION month_plan(int);
DROP TABLE events;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< agg_group_open_cost

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
