-- Lookup, semi, anti and inner joins of an aggregate scan held to the normal plan: IN and range conditions on the joined table, keys of every integer type, a join that returns no column of the table, a lookup table keyed by three text columns, prepared statements and the keys of an IN list.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
\set VERBOSITY terse

-- the rows of q as sorted jsonb text, or its error, with the aggregate scan on or off
CREATE FUNCTION mig_run(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;
-- 'same' when the rows (or the error) of q are those of the normal plan, else both cut to 200 characters
CREATE FUNCTION mig_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := mig_run(q, false); b text := mig_run(q, true);
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;
-- the control: a statement whose rows depend on the setting is told apart
SELECT mig_same($$SELECT current_setting('sift.enable_agg') AS v$$) LIKE 'DIFF%' AS control_detects_a_difference;

-- >>> in_list
SET client_min_messages = warning;
CREATE SCHEMA m1;
GRANT USAGE ON SCHEMA m1 TO PUBLIC;
SET search_path = m1, public;
-- The joined table of a lookup join of Custom Scan (SiftAgg) is read through a btree for its own `column IN (list)` conditions as for `column = value`
-- (sift.agg_lookup_pushdown): a list that keeps a small share of the table is an index scan, a list that keeps a large share, NOT IN and a table without
-- an index are read by the heap scan, with the same rows. Every statement is run with the pushdown, without it and with sift.enable_agg = off.
SET work_mem = '64MB';
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
-- the statements check the lookup tables the scan loads (the plans with probes have their own test, agg_probe_rewrite)
-- the ordered index-only scan of the key joined by a memoized lookup costs few pages and would take the plan over from the pushed one at these page costs: the statements check the pushed plan
SET sift.enable_col_scan = off;
CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 30000 END, date '2024-01-01' + i % 700,
       (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 300000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, day, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
-- 100 countries (a list of a few keeps a few percent of the table), a city name that repeats, a tier, and the same rows without an index
CREATE TABLE dims (id int PRIMARY KEY, country int, tier int2, city text, active boolean);
INSERT INTO dims SELECT i, i % 100, (i % 5)::int2, 'c' || i % 700, i % 7 <> 0 FROM generate_series(1, 30000) i;
INSERT INTO dims VALUES (30001, NULL, 1, NULL, true), (30002, 5, 1, 'c5', true);
CREATE INDEX dims_country_tier ON dims (country, tier);
CREATE INDEX dims_city ON dims (city);
CREATE TABLE dims_bare (id int, country int, tier int2);
INSERT INTO dims_bare SELECT id, country, tier FROM dims;
ANALYZE dims;
ANALYZE dims_bare;
-- whether the plan is the pushed one, and what the load of the lookup tables read, kept and used
-- a short list of an indexed column: an inner join that returns a column, a semi join, an EXISTS, ANY of an array
SELECT mig_same('SELECT d.country, count(*) AS n, sum(amount) AS s FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (3, 17, 41) GROUP BY d.country') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT id FROM dims WHERE country IN (3, 17, 41)) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_a_id AND d.country = ANY (ARRAY[3, 17, 41])) GROUP BY kind') AS same;
-- a text list, the duplicates and a NULL of a list, values no row has, one value
SELECT mig_same('SELECT d.city, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.city IN (''c5'', ''c6'', ''c699'') GROUP BY d.city') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (3, 3, NULL, 17) GROUP BY d.country') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (500, 501) GROUP BY d.country') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (5) GROUP BY d.country') AS same;
-- the index (country, tier): a list on the first column and a value on the second, and a condition that is not a key
SELECT mig_same('SELECT d.country, d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (3, 17, 41) AND d.tier = 2 GROUP BY 1, 2') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (3, 17, 41) AND d.active GROUP BY 1') AS same;
SELECT mig_same('SELECT d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.tier IN (1, 2) AND d.country IN (3, 17) GROUP BY 1') AS same;
-- a LEFT join keeps the events without a match; the list is a condition of the ON clause
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev LEFT JOIN dims d ON d.id = ev.dim_a_id AND d.country IN (3, 17, 41) GROUP BY d.country') AS same;
-- a list that keeps half of the table is read by the heap scan, as a table with no index and NOT IN are
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49) GROUP BY d.country') AS same;
SELECT mig_same('SELECT b.country, count(*) AS n FROM ev JOIN dims_bare b ON b.id = ev.dim_a_id WHERE b.country IN (3, 17, 41) GROUP BY b.country') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country NOT IN (3, 17, 41) AND d.tier <> 4 GROUP BY d.country') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country IN (3, 17, 41) AND ev.day >= ''2025-01-01'' GROUP BY d.country') AS same;
-- a list that is a parameter: the generic plan reads the array when the scan starts
SET plan_cache_mode = force_generic_plan;
PREPARE by_countries(int[]) AS SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = ANY ($1) GROUP BY d.country ORDER BY 1;
EXECUTE by_countries(ARRAY[3, 17, 41]);
EXECUTE by_countries(ARRAY[3, NULL]);
EXECUTE by_countries('{}');
EXECUTE by_countries(NULL);
-- a cached generic plan does not follow the setting: prepare it again with the pushdown off
SET sift.enable_agg = off;
DEALLOCATE by_countries;
PREPARE by_countries(int[]) AS SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = ANY ($1) GROUP BY d.country ORDER BY 1;
EXECUTE by_countries(ARRAY[3, 17, 41]);
RESET sift.enable_agg;
RESET plan_cache_mode;
DEALLOCATE ALL;
DROP TABLE ev, dims, dims_bare;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m1 CASCADE;
-- <<< in_list

-- >>> range_bounds
SET client_min_messages = warning;
CREATE SCHEMA m2;
GRANT USAGE ON SCHEMA m2 TO PUBLIC;
SET search_path = m2, public;
-- The joined table of a lookup join of Custom Scan (SiftAgg) is read through a btree for its own `column < value`, `<=`, `>` and `>=` conditions (both bounds of a range) as for
-- `column = value` (sift.agg_lookup_pushdown): the bounds follow the equal columns of the index, a bound that keeps a small share of the table is an index scan, a bound that keeps a
-- large share, a table without an index and a condition that is not a bound are read by the heap scan, with the same rows. Every statement is run with the pushdown, without it and with
-- sift.enable_agg = off.
SET work_mem = '256MB';
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SET sift.enable_col_scan = off;
CREATE TABLE ev (id int PRIMARY KEY, dim_id int, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 30000 END, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0 FROM generate_series(1, 300000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_id, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
-- the id is the key of the join; country and tier are the equal columns of an index that a bound follows, the name is text under a collation, the stamp a date
CREATE TABLE dims (id int PRIMARY KEY, country int, tier int2, name text COLLATE "C", day date);
INSERT INTO dims SELECT i, i % 100, (i % 5)::int2, 'n' || lpad(i::text, 5, '0'), date '2024-01-01' + i % 900 FROM generate_series(1, 30000) i;
INSERT INTO dims VALUES (30001, NULL, NULL, NULL, NULL);
CREATE INDEX dims_country_tier ON dims (country, tier);
CREATE INDEX dims_name ON dims (name);
CREATE INDEX dims_day ON dims (day);
CREATE TABLE dims_bare (id int, country int, tier int2);
INSERT INTO dims_bare SELECT id, country, tier FROM dims;
ANALYZE dims;
ANALYZE dims_bare;
-- each bound of the key, both bounds, the bound of a semi join, an EXISTS, an inner join that returns a column
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 300) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id < 300) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id > 29700) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 29700) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 100 AND id < 400) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.id <= 300) GROUP BY kind') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n, sum(amount) AS s FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.id <= 300 GROUP BY d.country') AS same;
-- a bound that keeps no row, a bound that keeps one, the bounds that cross
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 0) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 30000 AND id <= 30000) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 300 AND id < 100) GROUP BY kind') AS same;
-- the index (country, tier): a value of the first column and a bound of the second, a bound of the first column, a list and a bound
SELECT mig_same('SELECT d.country, d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.country = 3 AND d.tier <= 3 GROUP BY 1, 2') AS same;
SELECT mig_same('SELECT d.country, d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.country IN (3, 17) AND d.tier >= 3 GROUP BY 1, 2') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.country < 2 GROUP BY 1') AS same;
SELECT mig_same('SELECT d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.tier < 4 AND d.country = 3 GROUP BY 1') AS same;
-- text under a collation of the column, a date, an operand of another type (bigint, a cast)
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE name < ''n00300'')') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE name >= ''n00200'' AND name <= ''n00400'')') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE name < ''n00300'' COLLATE "en-x-icu")') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE day < date ''2024-01-05'')') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 300::bigint)') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 300.5::numeric)') AS same;
-- a NULL operand matches no row; a NULL of the column never passes a bound
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= NULL::int)') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE country < 3)') AS same;
-- the bound of a LEFT join is a condition of its ON clause: the events keep their rows
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev LEFT JOIN dims d ON d.id = ev.dim_id AND d.id <= 300 GROUP BY d.country') AS same;
-- a bound that keeps most of the table is read by the heap scan, as a table with no index, a condition that is not a bound (<>, a function) and a bound of an expression are
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 20000) GROUP BY kind') AS same;
SELECT mig_same('SELECT b.country, count(*) AS n FROM ev JOIN dims_bare b ON b.id = ev.dim_id WHERE b.id <= 300 GROUP BY b.country') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <> 300) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id + 0 <= 300) GROUP BY kind') AS same;
-- a bound that is a parameter: the generic plan reads it when the scan starts
SET plan_cache_mode = force_generic_plan;
PREPARE up_to(int) AS SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= $1) GROUP BY kind ORDER BY 1;
EXECUTE up_to(300);
EXECUTE up_to(0);
EXECUTE up_to(NULL);
SET sift.enable_agg = off;
DEALLOCATE up_to;
PREPARE up_to(int) AS SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= $1) GROUP BY kind ORDER BY 1;
EXECUTE up_to(300);
RESET sift.enable_agg;
RESET plan_cache_mode;
DEALLOCATE ALL;
DROP TABLE ev, dims, dims_bare;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m2 CASCADE;
-- <<< range_bounds

-- >>> inner_semi
SET client_min_messages = warning;
CREATE SCHEMA m3;
GRANT USAGE ON SCHEMA m3 TO PUBLIC;
SET search_path = m3, public;
-- An inner lookup join of Custom Scan (SiftAgg) that returns no column of the table (a dimension that only filters: `JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = 7`)
-- is a membership test when no key of the loaded rows repeats: the byte table of the semi join answers it (`Semi Lookup Tables`). A repeated key, a column of
-- the table that the query reads, a LEFT join and a text key keep the generic join, which repeats a tuple per matching row. Every statement is run with the
-- byte table, with sift.agg_semi_lut = off and with sift.enable_agg = off: the rows must be the same.
SET work_mem = '64MB';
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
-- the statements check the lookup tables the scan loads (the plans with probes have their own test, agg_probe_rewrite)
-- the ordered index-only scan of the key joined by a memoized lookup costs few pages and would take the plan over from the pushed one at these page costs: the statements check the pushed plan
SET sift.enable_col_scan = off;
CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 30000 END, date '2024-01-01' + i % 700,
       (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 300000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, day, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
-- a dimension with a unique key, the same with a repeated key and a NULL key, and the same keys as text
CREATE TABLE dims (id int PRIMARY KEY, country int, tier int2, active boolean);
INSERT INTO dims SELECT i, i % 60, (i % 5)::int2, i % 7 <> 0 FROM generate_series(1, 30000) i;
CREATE TABLE dims_dup (id int, country int, tier int2);
INSERT INTO dims_dup SELECT id, country, tier FROM dims;
INSERT INTO dims_dup VALUES (11, 11, 3), (NULL, 11, 1), (900000, 11, 1), (-5, 11, 1), (14, 14, 0);
CREATE TABLE dims_text (id text, country int);
INSERT INTO dims_text SELECT id::text, country FROM dims;
CREATE TABLE dims8 (id int8, country int);
INSERT INTO dims8 SELECT id, country FROM dims;
INSERT INTO dims8 VALUES (5000000000, 7), (-5000000000, 7);
ANALYZE dims;
ANALYZE dims_dup;
ANALYZE dims_text;
ANALYZE dims8;
-- the dimension only filters: one country, half of the table, its own conditions on several columns, a second condition on the events
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = 7 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country < 30 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country < 30 AND d.tier >= 2 AND d.active AND day >= ''2025-01-01'' GROUP BY kind') AS same;
SELECT mig_same('SELECT count(*) AS n, min(day) AS d0 FROM ev JOIN dims d ON d.id = ev.dim_a_id AND d.country = 7') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = 999') AS same;
-- two dimensions, one of them only filters (PostgreSQL 17 plans one semi table more than 18: the label is not printed)
-- a repeated key repeats the tuple: the generic join
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims_dup d ON d.id = ev.dim_a_id WHERE d.country = 11 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims_dup d ON d.id = ev.dim_a_id WHERE d.country = 14 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims_dup d ON d.id = ev.dim_a_id WHERE d.country = 13 GROUP BY kind') AS same;
-- the rows that hold the condition have no repeated key although the table has (the table is unique among the kept rows)
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims_dup d ON d.id = ev.dim_a_id WHERE d.country = 12 AND d.tier = 2 GROUP BY kind') AS same;
-- a column the query reads, as a key, an aggregate argument or an aggregate filter
SELECT mig_same('SELECT d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = 7 GROUP BY d.tier') AS same;
SELECT mig_same('SELECT count(d.tier) AS n, sum(d.country) AS s FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country < 5') AS same;
SELECT mig_same('SELECT count(*) FILTER (WHERE d.tier = 2) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country < 5') AS same;
-- a LEFT join keeps the events without a row, a text key and an int8 key (keys past int4 match no event) keep the generic join
SELECT mig_same('SELECT count(d.id) AS n, count(*) AS t FROM ev LEFT JOIN dims d ON d.id = ev.dim_a_id AND d.country = 7') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims_text d ON d.id = ev.dim_a_id::text WHERE d.country = 7 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dims8 d ON d.id = ev.dim_a_id WHERE d.country = 7 GROUP BY kind') AS same;
-- a prepared statement with a generic plan
SET plan_cache_mode = force_generic_plan;
PREPARE by_country(int) AS SELECT kind, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = $1 GROUP BY kind ORDER BY kind;
EXECUTE by_country(7);
EXECUTE by_country(999);
EXECUTE by_country(NULL);
SET sift.enable_agg = off;
EXECUTE by_country(7);
RESET sift.enable_agg;
RESET plan_cache_mode;
DEALLOCATE ALL;
DROP TABLE ev, dims, dims_dup, dims_text, dims8;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m3 CASCADE;
-- <<< inner_semi

-- >>> join_key_types
SET client_min_messages = warning;
CREATE SCHEMA m4;
GRANT USAGE ON SCHEMA m4 TO PUBLIC;
SET search_path = m4, public;
-- The keys of the joined table of an inner or semi lookup join are a conjunct of the scan (Join Key Filters) for an int8 leaf and for int4 keys that span more
-- than the byte table can hold (a hash table of the keys). A join that returns no column of the table and whose keys are unique then passes every tuple
-- that the filter let through (Join Key Passes), where the byte table of a semi join (Semi Lookup Tables) cannot hold the keys. Every statement is run as it is, with sift.agg_join_filter = off and with sift.enable_agg = off:
-- the rows must be the same.
SET work_mem = '64MB';
SET client_min_messages = warning;
SET default_statistics_target = 10000;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
-- the statements check the lookup tables the scan loads (the plans with probes have their own test, agg_probe_rewrite)
-- the ordered index-only scan of the key joined by a memoized lookup costs few pages and would take the plan over from the pushed one at these page costs: the statements check the pushed plan
SET sift.enable_col_scan = off;
-- events with an int8 key, a sparse int4 key (a range past 4 million values) and a dense int4 key
CREATE TABLE ev (id int PRIMARY KEY, k8 int8, ks int4, kd int4, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL WHEN i % 991 = 0 THEN -4611686018427387904 + i ELSE 1 + (i::bigint * 7919) % 30000 END,
       CASE WHEN i % 997 = 0 THEN NULL ELSE (1 + (i::bigint * 7919) % 30000)::int * 1000 END,
       CASE WHEN i % 997 = 0 THEN NULL ELSE (1 + (i::bigint * 7919) % 30000)::int END,
       (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 300000) i;
INSERT INTO ev VALUES (300001, 4611686018427387904, 2147483647, 2147483647, 'a', 1), (300002, 30001, -2147483648, -2147483648, 'b', 2);
CREATE INDEX ev_col ON ev USING sift_col (k8, ks, kd, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
-- dimensions: int8 keys with a key past int4 and a negative one, int4 keys 1000 apart, a dense int4 key, a repeated int8 key
CREATE TABLE d8 (id int8 PRIMARY KEY, country int, label text);
INSERT INTO d8 SELECT i, i % 60, 'e' || i FROM generate_series(1, 30000) i;
INSERT INTO d8 VALUES (4611686018427387904, 7, 'far'), (-4611686018427387904 + 991, 7, 'neg'), (30001, 7, 'edge');
CREATE TABLE ds (id int PRIMARY KEY, country int, label text);
INSERT INTO ds SELECT i * 1000, i % 60, 'e' || i FROM generate_series(1, 30000) i;
INSERT INTO ds VALUES (2147483647, 7, 'max'), (-2147483648, 7, 'min');
CREATE TABLE dd (id int PRIMARY KEY, country int, label text);
INSERT INTO dd SELECT i, i % 60, 'e' || i FROM generate_series(1, 30000) i;
CREATE TABLE d8_dup (id int8, country int);
INSERT INTO d8_dup SELECT i, i % 60 FROM generate_series(1, 30000) i;
INSERT INTO d8_dup VALUES (11, 11), (NULL, 11);
CREATE TABLE d4_for8 (id int4, country int);
INSERT INTO d4_for8 SELECT i, i % 60 FROM generate_series(1, 30000) i;
ANALYZE d8;
ANALYZE ds;
ANALYZE dd;
ANALYZE d8_dup;
ANALYZE d4_for8;
-- an int8 leaf: the joined columns are read, the table only filters, a semi join, EXISTS, a key past int4, a negative one, NULL leaves
SELECT mig_same('SELECT d.label, count(*) AS n FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.country = 7 GROUP BY d.label ORDER BY 2 DESC, 1 LIMIT 5') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.country < 30 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE k8 IN (SELECT id FROM d8 WHERE country < 30) GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE EXISTS (SELECT 1 FROM d8 d WHERE d.id = ev.k8 AND d.country = 7) GROUP BY kind') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.label IN (''far'', ''neg'', ''edge'')') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.country = 999 GROUP BY kind') AS same;
SELECT mig_same('SELECT count(d.country) AS n, sum(d.country) AS s FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.country < 30') AS same;
-- a repeated key repeats the tuple, a NOT EXISTS keeps the events without a row: no tuple is passed untested
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN d8_dup d ON d.id = ev.k8 WHERE d.country = 11 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM d8 d WHERE d.id = ev.k8 AND d.country < 30) GROUP BY kind') AS same;
SELECT mig_same('SELECT d.country, count(*) AS n FROM ev LEFT JOIN d8 d ON d.id = ev.k8 AND d.country = 7 GROUP BY d.country') AS same;
-- int4 keys 1000 apart (a range of 30 million values), with the keys at both ends of int4
SELECT mig_same('SELECT d.label, count(*) AS n FROM ev JOIN ds d ON d.id = ev.ks WHERE d.country = 7 GROUP BY d.label ORDER BY 2 DESC, 1 LIMIT 5') AS same;
SELECT mig_same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev JOIN ds d ON d.id = ev.ks WHERE d.country < 30 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev WHERE ks IN (SELECT id FROM ds WHERE country < 30 OR country = 7) GROUP BY kind') AS same;
SELECT mig_same('SELECT count(*) AS n FROM ev JOIN ds d ON d.id = ev.ks WHERE d.label IN (''max'', ''min'')') AS same;
-- the dense key keeps the byte table
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN dd d ON d.id = ev.kd WHERE d.country < 30 GROUP BY kind') AS same;
-- an int4 column against an int8 leaf, an int8 column against an int4 leaf
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN d4_for8 d ON d.id = ev.k8 WHERE d.country < 30 GROUP BY kind') AS same;
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN d8 d ON d.id = ev.kd WHERE d.country < 30 GROUP BY kind') AS same;
-- the events' own conditions next to the join, a prepared statement with a generic plan
SELECT mig_same('SELECT kind, count(*) AS n FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.country < 30 AND kind <> ''a'' AND amount > 20 GROUP BY kind') AS same;
SET plan_cache_mode = force_generic_plan;
PREPARE by_country(int) AS SELECT kind, count(*) AS n FROM ev JOIN d8 d ON d.id = ev.k8 WHERE d.country = $1 GROUP BY kind ORDER BY kind;
EXECUTE by_country(7);
EXECUTE by_country(999);
SET sift.enable_agg = off;
EXECUTE by_country(7);
RESET sift.enable_agg;
RESET plan_cache_mode;
DEALLOCATE ALL;
DROP TABLE ev, d8, ds, dd, d8_dup, d4_for8;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m4 CASCADE;
-- <<< join_key_types

-- >>> semi_lut
SET client_min_messages = warning;
CREATE SCHEMA m5;
GRANT USAGE ON SCHEMA m5 TO PUBLIC;
SET search_path = m5, public;
-- A semi or anti join (IN (SELECT ..), EXISTS, NOT EXISTS) on an integer leaf of Custom Scan (SiftAgg) is answered from a byte table over the
-- keys of the joined table, built when the scan starts (sift.agg_semi_lut, work_mem; docs/SET-FILTERS.md). Every statement
-- is run with the table, with sift.agg_semi_lut = off (the generic lookup join) and with sift.enable_agg = off (the normal plan): the rows must be the same.
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
-- page costs that keep the pushed plan the cheaper one whenever it is generated: the plan then depends on the shape of the statement only
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
-- the statements check the lookup tables the scan loads (the plans with probes have their own test, agg_probe_rewrite)
-- the scan's key filter tests every key of a semi join the table would test (agg_join_key_types): left out, the table is built and used
-- the ordered index-only scan of the key joined by a memoized lookup costs few pages and would take the plan over from the pushed one at these page costs: the statements check the pushed plan
SET sift.enable_col_scan = off;
CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, small_id int2, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL WHEN i <= 5 THEN -i WHEN i <= 10 THEN 2147483647 - i ELSE 1 + (i::bigint * 7919) % 30000 END,
       (i % 50)::int2, date '2024-01-01' + i % 700, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 300000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, small_id, day, kind, amount);
-- the statistics of dim_a_id see every row (300 rows of sample per unit of target: 300000): sift reads the span of the values from them (a table over a span past
-- 4M values is not built), and a sample that misses the few rows far from the rest would let the plan of NOT IN (SELECT ..) flip from run to run
ALTER TABLE ev ALTER COLUMN dim_a_id SET STATISTICS 1000;
VACUUM (FREEZE, ANALYZE) ev;
-- sets of keys: a group membership (soft deleted, a NULL key, a duplicate), an attribute table, int2 and date keys, int8 keys, a table with no rows
CREATE TABLE dim_b (group_id int, dim_a_id int, deleted_at timestamptz);
INSERT INTO dim_b SELECT g, 1 + (g * 1000 + i * 37) % 30000, CASE WHEN i % 10 = 0 THEN now() END
FROM generate_series(1, 8) g, generate_series(1, 500 * g) i;
INSERT INTO dim_b VALUES (3, NULL, NULL), (3, 11, NULL), (3, 11, NULL), (9, -2, NULL), (9, -3, NULL), (9, 2147483640, NULL), (9, 2147483639, NULL), (9, 5, NULL);
CREATE TABLE dims (id int PRIMARY KEY, country int, tier int2, active boolean);
INSERT INTO dims SELECT i, i % 60, (i % 5)::int2, i % 7 <> 0 FROM generate_series(1, 30000) i;
CREATE TABLE codes (code int2, label text);
INSERT INTO codes SELECT i::int2, 'c' || i FROM generate_series(1, 50, 3) i;
CREATE TABLE holidays (d date);
INSERT INTO holidays SELECT date '2024-01-01' + i * 11 FROM generate_series(0, 60) i;
CREATE TABLE dim_b8 (dim_a_id int8);
INSERT INTO dim_b8 SELECT 1 + i * 13 FROM generate_series(0, 2000) i;
INSERT INTO dim_b8 VALUES (5000000000), (-5000000000), (4294967297), (NULL), (-3000000000);
CREATE TABLE dim_b_nn (dim_a_id int NOT NULL);
INSERT INTO dim_b_nn SELECT 1 + i * 17 FROM generate_series(0, 1500) i;
CREATE TABLE nothing (dim_a_id int);
CREATE TABLE sparse (dim_a_id int);
INSERT INTO sparse SELECT generate_series(1, 2000) UNION ALL SELECT 3000000;
ANALYZE dim_b;
ANALYZE dims;
ANALYZE codes;
ANALYZE holidays;
ANALYZE dim_b8;
ANALYZE dim_b_nn;
ANALYZE nothing;
ANALYZE sparse;
-- the byte tables of an EXPLAIN ANALYZE, and whether the plan is the pushed one
-- IN, EXISTS and NOT EXISTS over a membership table (its own conditions: the group, the soft delete), a NULL key and a duplicate; the whole table
-- (the last two) spans more than the byte table holds (group 9: keys from -3 to 2147483640): a hash set of the keys
SELECT mig_same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 3 AND deleted_at IS NULL) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = ev.dim_a_id AND m.group_id = 5) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE NOT EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = ev.dim_a_id AND m.group_id = 3) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT count(*) AS n, count(dim_a_id) AS c FROM ev WHERE NOT EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = ev.dim_a_id)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b)$$) AS same;
-- a dimension attribute: the keys of the rows of a table that have it
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT id FROM dims WHERE country = 7) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT id FROM dims WHERE country IN (7, 8, 9) AND tier >= 2 AND active) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_a_id AND d.country < 30) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_a_id AND NOT d.active) GROUP BY kind$$) AS same;
-- two semi joins and an anti join in one query, and with the filters of the leaves
SELECT mig_same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE day >= '2024-06-01' AND small_id < 40
	AND dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 6) AND dim_a_id IN (SELECT id FROM dims WHERE country < 40)
	AND NOT EXISTS (SELECT 1 FROM dim_b_nn x WHERE x.dim_a_id = ev.dim_a_id) GROUP BY kind$$) AS same;
-- int2 and date keys, and keys of another integer type
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE small_id IN (SELECT code FROM codes) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE small_id NOT IN (SELECT code FROM codes) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT count(*) AS n, sum(amount) AS s FROM ev WHERE day IN (SELECT d FROM holidays)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n, sum(amount) AS s FROM ev WHERE NOT EXISTS (SELECT 1 FROM holidays h WHERE h.d = ev.day)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b8)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n, count(dim_a_id) AS c FROM ev WHERE NOT EXISTS (SELECT 1 FROM dim_b8 m WHERE m.dim_a_id = ev.dim_a_id)$$) AS same;
-- NOT IN of a table whose key can be NULL stays a hashed subplan, asked by the scan for each tuple (the NULL makes the answer empty); of NOT NULL columns it is the same answer
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM dim_b8)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM dim_b_nn)$$) AS same;
-- a table with no rows, and no row that passes
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM nothing)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM nothing x WHERE x.dim_a_id = ev.dim_a_id)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id IN (SELECT id FROM dims WHERE country = 99)$$) AS same;
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_a_id AND d.country = 99)$$) AS same;
-- a condition of the table that reads the leaf (a table per tuple: the generic join), and an inner join as well
SELECT mig_same($$SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE ev.dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 4) GROUP BY d.country$$) AS same;
-- the span of the keys is bounded by work_mem: wider keys are a hash set of them while that fits the same memory, else the generic join; the answer is the same
SET work_mem = '1MB';
SELECT mig_same($$SELECT count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM sparse)$$) AS same;
RESET work_mem;
-- from workers: each builds its table (the scan reads every unit, so that the workers share a scan that pays for them)
ALTER TABLE ev SET (parallel_workers = 2);
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT mig_same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 8) GROUP BY kind$$) AS same;
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_a_id AND d.country < 30) GROUP BY kind$$) AS same;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
ALTER TABLE ev RESET (parallel_workers);
-- a rescan (the inner side of a nested loop) replays the same groups; a prepared statement with the parameter in the subquery, custom and generic
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT g, count(*) AS n FROM generate_series(1, 3) g CROSS JOIN LATERAL (SELECT kind FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 2) GROUP BY kind) k GROUP BY g ORDER BY g;
RESET enable_material;
RESET enable_hashjoin;
RESET enable_mergejoin;
PREPARE by_group(int) AS SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = $1) GROUP BY kind ORDER BY kind;
CREATE FUNCTION same_group(g int) RETURNS TABLE (result_rows bigint, vs_generic_join bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS EXECUTE by_group(%s)', g);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_gen ON COMMIT DROP AS EXECUTE by_group(%s)', g);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_generic_join := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_gen) UNION ALL (TABLE r_gen EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_gen;
	RETURN NEXT;
END $$;
SET plan_cache_mode = force_custom_plan;
SELECT * FROM same_group(2);
SET plan_cache_mode = force_generic_plan;
SELECT * FROM same_group(2);
SELECT * FROM same_group(7);
RESET plan_cache_mode;
-- the table is read under the snapshot of the scan: this transaction's own deletes, inserts and updates of the membership table
BEGIN;
DELETE FROM dim_b WHERE group_id = 5 AND dim_a_id % 2 = 0;
INSERT INTO dim_b SELECT 5, 20000 + i, NULL FROM generate_series(1, 300) i;
UPDATE dim_b SET deleted_at = now() WHERE group_id = 3 AND dim_a_id < 100;
SELECT mig_same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id IN (3, 5) AND deleted_at IS NULL) GROUP BY kind$$) AS same;
ROLLBACK;
DEALLOCATE ALL;
DROP TABLE ev, dim_b, dims, codes, holidays, dim_b8, dim_b_nn, nothing, sparse;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m5 CASCADE;
-- <<< semi_lut

-- >>> in_keyed
SET client_min_messages = warning;
CREATE SCHEMA m6;
GRANT USAGE ON SCHEMA m6 TO PUBLIC;
SET search_path = m6, public;
CREATE FUNCTION answers(keys text, kind text, mode text, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', (mode <> 'normal')::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || join_q(keys, kind) || ') t' INTO r;
	RETURN coalesce(r, '');
END $$;
-- A table joined by a column that the scan's IN list names is read by one btree scan with the list as its keys (and the join's `column = value` conditions on the index's next
-- columns) while that costs less than the table read whole: the rows read are those of the keys, the answers the same as with the table read whole, in a serial and a parallel scan.
SET client_min_messages = warning;
SET jit = off;
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
-- the statements check the keyed load (the plans with probes have their own test, agg_probe_rewrite)
CREATE TABLE ev (id int, fk int, v int, d date) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 1000, i % 7, date '2026-01-01' + i % 300 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, fk, v, d);
-- twenty-five rows to a key, five to each status: a thousand pages in all
CREATE TABLE lk (id int, parent int, status int, label text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO lk SELECT i, i % 1000, (i / 1000) % 5, 'l' || (i % 1000 % 3), repeat('x', 300) FROM generate_series(0, 24999) i;
CREATE INDEX lk_parent_status ON lk (parent, status);
CREATE INDEX lk_parent ON lk (parent);
-- the columns of the join in the entries: no heap fetch on an all-visible page
CREATE INDEX lk_cover ON lk (parent, status) INCLUDE (label);
VACUUM (FREEZE, ANALYZE) ev, lk;
-- the rows read, the index scans and the heap fetches of the scan, from its EXPLAIN
CREATE FUNCTION join_q(keys text, kind text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE kind
		WHEN 'inner' THEN format('SELECT lk.label, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk IN (%s) GROUP BY 1 ORDER BY 1', keys)
		WHEN 'inner_all' THEN format('SELECT lk.label, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk WHERE ev.fk IN (%s) GROUP BY 1 ORDER BY 1', keys)
		WHEN 'semi' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.fk IN (%s) AND EXISTS (SELECT 1 FROM lk WHERE lk.parent = ev.fk AND lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'anti' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.fk IN (%s) AND NOT EXISTS (SELECT 1 FROM lk WHERE lk.parent = ev.fk AND lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'left' THEN format('SELECT lk.label, count(*), sum(ev.v) FROM ev LEFT JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk IN (%s) GROUP BY 1 ORDER BY 1', keys)
	END
$$;
SELECT string_agg(i::text, ',') AS few FROM generate_series(10, 14) i \gset
SELECT string_agg(i::text, ',') AS many FROM generate_series(0, 999) i \gset
SELECT string_agg(i::text, ',') AS some FROM generate_series(100, 130) i \gset
-- five keys: their twenty-five entries (the index names the status, five rows of the twenty-five), one index scan
-- without a condition of the join's own: the twenty-five rows of each key
-- thirty-one keys: still by the index
-- every key: the five thousand entries of the status by the index; with the twenty-five thousand rows of the table all wanted the covering btree still gives them from its entries (all-visible pages: 2.5 ms to load against 7.4 for the table read whole, 25,000 rows of 1,087 pages)
-- rows changed since the last VACUUM: their pages are not all-visible, those entries are fetched from the heap (and a deleted row, an updated label, are not those of the entries)
UPDATE lk SET label = 'l1' WHERE parent BETWEEN 10 AND 12 AND status = 2 AND id % 2 = 0;
DELETE FROM lk WHERE parent = 13 AND status = 2;
-- the answers: the same with the table read whole (the setting off) and from the normal plan, serial and parallel; keys listed twice, a key the table has no row for and a NULL in the list
SELECT k.name, kind, w.workers, answers(k.keys, kind, 'keyed', w.workers) = answers(k.keys, kind, 'whole', w.workers) AND answers(k.keys, kind, 'keyed', w.workers) = answers(k.keys, kind, 'normal', w.workers) AS same_answers,
	   answers(k.keys, kind, 'keyed', w.workers) <> '' AS not_empty
FROM (VALUES ('few', :'few'), ('some', :'some'), ('twice', '10,10,11,11,12'), ('absent', '10,5000,11'), ('null', '10,NULL,11'), ('one', '7'), ('many', :'many')) k(name, keys),
	 (VALUES ('inner'), ('inner_all'), ('semi'), ('anti'), ('left')) q(kind), (VALUES (0), (3)) w(workers)
ORDER BY 1, 2, 3;
DROP TABLE ev, lk;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m6 CASCADE;
-- <<< in_keyed

-- >>> combo_hash
SET client_min_messages = warning;
CREATE SCHEMA m7;
GRANT USAGE ON SCHEMA m7 TO PUBLIC;
SET search_path = m7, public;
-- A lookup join keyed by three text columns makes one combo entry per distinct combination of the fact values. Tens of thousands of combinations
-- of small dictionary ids must spread over the entry table (they once piled up in a few slots, a probe walked thousands of entries): the rows are
-- the same as the normal plan's, and the node is the aggregate scan.
SET work_mem = '256MB';
SET default_statistics_target = 10000;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
-- the statements check the lookup tables the scan loads (the plans with probes have their own test, agg_probe_rewrite)
CREATE TABLE ev (id int PRIMARY KEY, kind text, bucket text, sub text, day date, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, 'k' || (abs(hashint4(i)) % 40), 'b' || (abs(hashint4(i + 1000000)) % 60), 's' || (abs(hashint4(i + 2000000)) % 50), date '2024-01-01' + i % 700, (i % 1000) / 10.0
FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (kind, bucket, sub, day, amount);
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE maps (kind text, bucket text, sub text, label text) WITH (autovacuum_enabled = false);
INSERT INTO maps
SELECT 'k' || k, 'b' || b, 's' || s, 'L' || ((k + b + s) % 25)
FROM generate_series(0, 39) k, generate_series(0, 59) b, generate_series(0, 49) s
WHERE (k * 7 + b * 3 + s) % 5 <> 0;
CREATE INDEX maps_idx ON maps (kind, bucket, sub);
VACUUM (FREEZE, ANALYZE) maps;
SELECT count(DISTINCT (kind, bucket, sub)) > 40000 AS many_combinations FROM ev;
\set q 'SELECT coalesce(m.label, ''none'') AS label, count(*) AS n, sum(ev.amount) AS total FROM ev LEFT JOIN maps m ON m.kind = ev.kind AND m.bucket = ev.bucket AND m.sub = ev.sub WHERE ev.day >= date ''2024-03-01'' GROUP BY 1'
CREATE TEMP TABLE r_on AS :q;
SET sift.enable_agg = off;
CREATE TEMP TABLE r_off AS :q;
RESET sift.enable_agg;
SELECT count(*) AS labels, (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d) AS differences FROM r_on;
SELECT label, n FROM r_on WHERE label IN ('none', 'L0', 'L24') ORDER BY label;
DROP TABLE ev, maps;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m7 CASCADE;
-- <<< combo_hash

DROP FUNCTION mig_same(text), mig_run(text, bool);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;

