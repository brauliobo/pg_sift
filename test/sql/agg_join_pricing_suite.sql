-- The joins and probes of an aggregate scan. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_join_chain_load_price
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- A lookup table that depends on another (the tenants' datasets, whose rows a fact reaches through the dimension that names them) is loaded for the keys
-- of the rows the other keeps, by its index on the column the join names: the scan is priced for those rows, not for the whole table. The price of the
-- scan of a chain of three lookups restricted to one tenant is a fraction of the price of the same chain over every tenant.

SET client_min_messages = warning;

CREATE TABLE ch_tenant (id int PRIMARY KEY);
CREATE TABLE ch_set (id int PRIMARY KEY, tenant_id int NOT NULL);
CREATE TABLE ch_dim (id int PRIMARY KEY, set_id int NOT NULL, attr int NOT NULL);
CREATE TABLE ch_fact (id int PRIMARY KEY, dim_id int NOT NULL, val int NOT NULL);
INSERT INTO ch_tenant SELECT g FROM generate_series(1, 40) g;
INSERT INTO ch_set SELECT g, 1 + g % 40 FROM generate_series(1, 1000) g;
INSERT INTO ch_dim SELECT g, 1 + g % 1000, g % 4 FROM generate_series(1, 25000) g;
INSERT INTO ch_fact SELECT g, 1 + (g::bigint * 7919) % 25000, g % 1000 FROM generate_series(1, 100000) g;
CREATE INDEX ch_set_tenant ON ch_set (tenant_id, id);
CREATE INDEX ch_dim_set ON ch_dim (set_id);
CREATE INDEX ch_tenant_col ON ch_tenant USING sift_col (id);
CREATE INDEX ch_set_col ON ch_set USING sift_col (id, tenant_id);
CREATE INDEX ch_dim_col ON ch_dim USING sift_col (id, set_id, attr);
CREATE INDEX ch_fact_col ON ch_fact USING sift_col (id, dim_id, val);
VACUUM (FREEZE, ANALYZE) ch_tenant, ch_set, ch_dim, ch_fact;

CREATE FUNCTION ch_cost(q text) RETURNS float8 LANGUAGE sql AS $$ SELECT scan_cost FROM sift.explain_why(q) $$;

SELECT ch_cost('SELECT d.attr, sum(f.val) FROM ch_fact f JOIN ch_dim d ON d.id = f.dim_id JOIN ch_set s ON s.id = d.set_id JOIN ch_tenant t ON t.id = s.tenant_id WHERE t.id = 3 GROUP BY 1')
	< 0.6 * ch_cost('SELECT d.attr, sum(f.val) FROM ch_fact f JOIN ch_dim d ON d.id = f.dim_id JOIN ch_set s ON s.id = d.set_id JOIN ch_tenant t ON t.id = s.tenant_id GROUP BY 1')
	AS one_tenant_is_priced_for_its_rows;

DROP FUNCTION ch_cost(text);
DROP TABLE ch_fact, ch_dim, ch_set, ch_tenant;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_join_chain_load_price

-- >>> agg_join_filter_elem_table
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- The inner or semi lookup join on a text field of the elements of an array tests the keys of the table on the encoded form of each unit; a sift_table runs the test of the
-- element field through the group filter, as a column index does (it used to stop with "leaf is an element column"). The rows must be those of the normal plan, with no worker and with two.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE et (id int, grp int, items jsonb, amount int) USING sift_table;
SELECT sift.table_set_paths('et', 'items', 'items[]{category text, qty int4}');
-- no array, an empty array, an element with a missing field and one with a NULL field
INSERT INTO et
SELECT i, i % 7,
       CASE WHEN i % 19 = 0 THEN NULL
            WHEN i % 29 = 0 THEN '[]'::jsonb
            ELSE (SELECT jsonb_agg(CASE WHEN (i + e) % 31 = 0 THEN '{"qty": 1}'::jsonb
                                        WHEN (i + e) % 23 = 0 THEN jsonb_build_object('category', NULL, 'qty', e)
                                        ELSE jsonb_build_object('category', 'cat' || (i + e) % 40, 'qty', e) END)
                  FROM generate_series(1, 1 + i % 4) e) END,
       i % 100
FROM generate_series(1, 6000) i;
VACUUM FULL et;
CREATE TABLE keys (k text, tag int);
INSERT INTO keys VALUES ('cat3', 1), ('cat3', 2), ('cat17', 3), ('cat39', 4), (NULL, 5), ('absent', 6);
CREATE TABLE keys_uniq (k text PRIMARY KEY, tag int);
INSERT INTO keys_uniq VALUES ('cat3', 1), ('cat17', 3), ('cat39', 4), ('absent', 6);
ANALYZE keys; ANALYZE keys_uniq;

CREATE TABLE q (n int, label text, q text);
INSERT INTO q VALUES
	(1, 'a semi join', 'SELECT i.category, count(*) AS n, sum(i.qty) AS units FROM et o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) WHERE EXISTS (SELECT 1 FROM keys m WHERE m.k = i.category) GROUP BY 1 ORDER BY 1'),
	(2, 'an inner join by a unique key', 'SELECT m.tag, count(*) AS n, count(DISTINCT o.id) AS orders FROM et o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) JOIN keys_uniq m ON m.k = i.category GROUP BY 1 ORDER BY 1'),
	(3, 'a semi join and a condition on the table', 'SELECT i.category, count(*) AS n FROM et o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) WHERE o.grp < 3 AND EXISTS (SELECT 1 FROM keys m WHERE m.k = i.category) GROUP BY 1 ORDER BY 1');
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_with_text_key_filters(q) $$;
CREATE FUNCTION elem_rows(q text, scan bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers_reset(q, scan, workers) $$;
SELECT n, label, plan_of(q) AS plan, elem_rows(q, false, 0) <> '' AS has_rows, elem_rows(q, true, 0) = elem_rows(q, false, 0) AS same, elem_rows(q, true, 2) = elem_rows(q, false, 0) AS same_workers FROM q ORDER BY n;

DROP FUNCTION plan_of(text);
DROP FUNCTION elem_rows(text, bool, int);
DROP TABLE q, et, keys, keys_uniq;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_join_filter_elem_table

-- >>> agg_join_filter_price
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- A join whose condition is no key (`k = n + 1`: an addition can raise) tests every tuple the scan keeps against every row of the joined table, so the scan
-- is priced for those tuples, not for the few the join lets through. With the page cost raised for the table scan, the plan with a column as the key stays the
-- scan and the one with the addition is the hash join, which runs in a fraction of the time. Every query gives the same rows either way.
SET work_mem = '64MB';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 20;

CREATE TABLE facts (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, i, i % 10 FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) facts;
CREATE TABLE keys (n int) WITH (autovacuum_enabled = false);
INSERT INTO keys SELECT i * 37 FROM generate_series(1, 200) i;
VACUUM (FREEZE, ANALYZE) keys;
-- 40,000 rows of which a filter keeps 20, and a fact table of 100 values
CREATE TABLE wide_keys (n int, g int) WITH (autovacuum_enabled = false);
INSERT INTO wide_keys SELECT i % 100, i % 2000 FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) wide_keys;
CREATE TABLE few (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO few SELECT i, i % 100, i % 10 FROM generate_series(1, 20000) i;
CREATE INDEX few_col ON few USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) few;

\ir test/agg_helpers.sql

SELECT sub, pushed(q) AS scan, runq(q, false) = runq(q, true) AS same_rows
FROM (SELECT sub, format('SELECT count(*), sum(v) FROM facts WHERE k IN (SELECT %s FROM keys)', sub) AS q
	  FROM unnest(ARRAY['n', 'n + 1', 'n::bigint']) sub) t;

-- the tests are for the rows of the joined table that its own filter keeps (20 of 40,000), not for all of them
SELECT pushed(q) AS scan, runq(q, false) = runq(q, true) AS same_rows
FROM (SELECT 'SELECT count(*), sum(v) FROM few WHERE k IN (SELECT n + 1 FROM wide_keys WHERE g = 7)' AS q) t;

DROP FUNCTION outcome, runq, runs_in_scan, pushed;
DROP TABLE facts, keys, wide_keys, few;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_join_filter_price

-- >>> agg_join_keys_only_price
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- A table of sparse keys (read as rows, not as classes) that only tells whether a key exists (EXISTS, NOT EXISTS, IN) is loaded as its keys alone: no directory of the keys is
-- built, so its load is priced by the rows it reads and not by the cache misses of a directory, from the heap or from a column index alike. At a million keys the scan
-- that loads it is the plan that stays (70-150 ms against 530-620 ms of the normal plan). The answers are those of the normal plan.
SET work_mem = '256MB';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE ko_heap (k int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO ko_heap SELECT g * 1000, g % 7 FROM generate_series(1, 50000) g;
CREATE TABLE ko_cols (k int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO ko_cols SELECT k, a FROM ko_heap;
CREATE TABLE ko_fact (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ko_fact SELECT g, 1000 * (1 + (g * 7) % 375000), g % 100 FROM generate_series(1, 50000) g;
SET client_min_messages = warning;
CREATE INDEX ko_cols_col ON ko_cols USING sift_col (k, a);
CREATE INDEX ko_fact_col ON ko_fact USING sift_col (id, k, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) ko_heap, ko_cols, ko_fact;

CREATE TABLE shapes (slug text, sql text);
INSERT INTO shapes
SELECT t || '_' || s, format(q, t)
FROM (VALUES ('heap'), ('cols')) tables (t),
     (VALUES ('exists', $q$SELECT sum(f.v) FROM ko_fact f WHERE EXISTS (SELECT 1 FROM ko_%1$s d WHERE d.k = f.k)$q$),
             ('not_exists', $q$SELECT sum(f.v) FROM ko_fact f WHERE NOT EXISTS (SELECT 1 FROM ko_%1$s d WHERE d.k = f.k)$q$),
             ('in', $q$SELECT sum(f.v) FROM ko_fact f WHERE f.k IN (SELECT k FROM ko_%1$s)$q$)) shape (s, q);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

-- the key is unique: PostgreSQL 19 plans an EXISTS or IN as an inner join (no "(semi)"), and the keys of an inner join are loaded alone as well (semi_lut_possible)
CREATE FUNCTION plan_of(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal plan'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins: ko_\w+( \((semi|anti)\))?$' THEN r := 'scan with the table loaded as keys'; END IF;
		IF l ~ 'sift_probe' THEN r := 'scan probing the table'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT slug, plan_of(sql) AS plan FROM shapes ORDER BY slug;
SELECT slug, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

DROP FUNCTION answer(text, bool), plan_of(text);
DROP TABLE shapes, ko_fact, ko_cols, ko_heap;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_join_keys_only_price

-- >>> agg_join_outer_param
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- A table joined to the output of an outer join, whose join key is a COALESCE of both sides, has parameterized paths that the outer join drives: the
-- relids that drive such a path include the outer join's own, which is no relation of the query and has no row estimate. Planning the grouped scan of
-- the table priced those paths and read the estimate of that relid (a crash, signal 11).
SET max_parallel_workers_per_gather = 0;
SET enable_hashjoin = off;
SET enable_mergejoin = off;

CREATE TABLE oj_assign (id int PRIMARY KEY, prop int, grp int) WITH (autovacuum_enabled = false);
INSERT INTO oj_assign SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE i % 500 END, i % 50 FROM generate_series(1, 2000) i;
CREATE TABLE oj_group (grp int, prop int) WITH (autovacuum_enabled = false);
INSERT INTO oj_group SELECT i % 50, (i * 7) % 500 FROM generate_series(1, 400) i;
CREATE TABLE oj_conn (id int, prop int, src int, kind text) WITH (autovacuum_enabled = false);
INSERT INTO oj_conn SELECT i, i % 500, i % 50, 'k' || i % 9 FROM generate_series(1, 30000) i;
SET client_min_messages = warning;
CREATE INDEX oj_conn_col ON oj_conn USING sift_col (id, prop, src, kind);
RESET client_min_messages;
CREATE INDEX oj_conn_prop ON oj_conn (prop);
VACUUM ANALYZE oj_assign, oj_group, oj_conn;

CREATE FUNCTION oj_rows() RETURNS TABLE (kind text, n bigint) LANGUAGE sql AS $$
	WITH sa AS (SELECT * FROM (SELECT a.id, coalesce(a.prop, g.prop) AS prop FROM oj_assign a LEFT JOIN oj_group g ON g.grp = a.grp
		WHERE coalesce(a.prop, g.prop) IS NOT NULL AND a.id < 30) t1)
	SELECT c.kind, count(*) FROM oj_conn c JOIN sa ON sa.prop = c.prop WHERE c.src < 5 GROUP BY 1 $$;

CREATE TABLE oj_with AS SELECT * FROM oj_rows();
SET sift.enable_agg = off;
CREATE TABLE oj_without AS SELECT * FROM oj_rows();
RESET sift.enable_agg;

SELECT count(*) > 0 AS has_rows, (SELECT count(*) FROM (SELECT * FROM oj_with EXCEPT SELECT * FROM oj_without) d) AS missing,
	(SELECT count(*) FROM (SELECT * FROM oj_without EXCEPT SELECT * FROM oj_with) d) AS extra FROM oj_with;

DROP FUNCTION oj_rows();
DROP TABLE oj_with, oj_without, oj_conn, oj_group, oj_assign;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_join_outer_param

-- >>> agg_join_probe_cost
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
-- A lookup join probes its table for every tuple the filters keep, before the joins narrow the rows to the groups, and evaluates the rest of its
-- ON clause for each distinct combination of the values it reads. A condition that compares the columns of both tables is tested per tuple: the
-- lookup table is loaded whole, so its keys cut no unit. The planner prices both, and leaves the normal plan where the lookup is the cost.
SET max_parallel_workers_per_gather = 0;

CREATE TABLE jp_items (id int PRIMARY KEY, kind text, size int) WITH (autovacuum_enabled = false);
INSERT INTO jp_items SELECT i, 'k' || i % 40, 1 + i % 50 FROM generate_series(1, 5000) i;
CREATE TABLE jp_sales (id int, item int, qty numeric(5,1), amount numeric(10,2), channel text) WITH (autovacuum_enabled = false);
INSERT INTO jp_sales SELECT i, 1 + (i::bigint * 7919) % 5000, 1 + i % 30, (i % 1000) / 10.0, (ARRAY['AIR', 'RAIL', 'TRUCK', 'SHIP', 'MAIL', 'FOB', 'REG AIR'])[1 + i % 7] FROM generate_series(1, 75000) i;
SET client_min_messages = warning;
CREATE INDEX jp_sales_col ON jp_sales USING sift_col (id, item, qty, amount, channel);
RESET client_min_messages;
CREATE INDEX jp_sales_item ON jp_sales (item);
VACUUM ANALYZE jp_items, jp_sales;

-- the plan the statement runs (explain_why also lists the arms of rewrites the planner did not keep: the form of an OR of conditions on the two tables as a join of the lookup
-- table's rows is priced, and not run, against the normal plan's 249,302)
CREATE FUNCTION scanned(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line LIKE '%Custom Scan (SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT q, used FROM (VALUES
	(1, $$SELECT sum(amount) FROM jp_sales s, jp_items i WHERE i.id = s.item AND ((i.kind = 'k3' AND s.qty >= 1 AND s.qty <= 11 AND i.size IN (1, 2, 3)) OR (i.kind = 'k7' AND s.qty >= 10 AND s.qty <= 20 AND i.size IN (4, 5, 6))) AND s.channel IN ('AIR', 'REG AIR')$$),
	(2, $$SELECT sum(amount) FROM jp_sales s, jp_items i WHERE i.id = s.item AND i.kind = 'k3'$$),
	(3, $$SELECT i.kind, sum(amount) FROM jp_sales s, jp_items i WHERE i.id = s.item GROUP BY 1$$)
) v(q, sql), LATERAL (SELECT scanned(v.sql) AS used) w ORDER BY q;

DROP FUNCTION scanned(text);
DROP TABLE jp_sales, jp_items;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_join_probe_cost

-- >>> agg_join_repeated_key_price
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
-- A table whose tests have few outcomes is kept as classes of its keys only if no key repeats (a class is the key's): a table with several rows for a key is read as rows into a
-- hash table of its keys, which costs a row each, past the cache a miss each. The planner prices the classes only for a table that can be one, and the directory of the keys of a
-- table of rows by its distinct keys. The answers are those of the normal plan.
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE rk_once (k int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO rk_once SELECT g, g % 7 FROM generate_series(1, 5000) g;
CREATE TABLE rk_many (id int, k int, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO rk_many SELECT g, 1 + g % 5000, (1 + g % 5000) % 7 FROM generate_series(1, 25000) g;
CREATE TABLE rk_fact (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO rk_fact SELECT g, 1 + (g * 7) % 5000, g % 100 FROM generate_series(1, 50000) g;
SET client_min_messages = warning;
CREATE INDEX rk_once_col ON rk_once USING sift_col (k, a);
CREATE INDEX rk_many_col ON rk_many USING sift_col (id, k, a);
CREATE INDEX rk_fact_col ON rk_fact USING sift_col (id, k, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) rk_once, rk_many, rk_fact;

CREATE FUNCTION scan_cost(t text) RETURNS float8 LANGUAGE sql AS $$
	SELECT scan_cost FROM sift.explain_why(format($q$SELECT sum(f.v) FILTER (WHERE d.a = 1), sum(f.v) FILTER (WHERE d.a < 3) FROM rk_fact f JOIN %s d ON d.k = f.k$q$, t))
$$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT scan_cost('rk_many') > 1.5 * scan_cost('rk_once') AS repeated_keys_cost_more;
SELECT answer(format($q$SELECT d.a, count(*), sum(f.v) FROM rk_fact f JOIN %s d ON d.k = f.k GROUP BY 1$q$, t), true) =
       answer(format($q$SELECT d.a, count(*), sum(f.v) FROM rk_fact f JOIN %s d ON d.k = f.k GROUP BY 1$q$, t), false) AS same_answers, t
FROM (VALUES ('rk_once'), ('rk_many')) v (t) ORDER BY t;

DROP FUNCTION scan_cost(text), answer(text, bool);
DROP TABLE rk_fact, rk_many, rk_once;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< agg_join_repeated_key_price

-- >>> agg_join_span_price
CREATE SCHEMA s8;
GRANT USAGE ON SCHEMA s8 TO PUBLIC;
SET search_path = s8, public;
-- A table is kept as an array of classes over its keys only if the keys span no more than 8 times its rows plus 65,536 (and the lookup budget): the load reads the whole index
-- before it finds that a sparser table does not, so the planner asks the same question (one function) and prices a sparser table as rows. The answers are those of the normal plan.
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE sp_dense (id int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO sp_dense SELECT g, g % 7 FROM generate_series(1, 20000) g;
-- the same rows with keys 100 apart: a span of 2,000,000, over 8 * 20,000 + 65,536
CREATE TABLE sp_wide (id int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO sp_wide SELECT g * 100, g % 7 FROM generate_series(1, 20000) g;
CREATE TABLE sp_fact_dense (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO sp_fact_dense SELECT g, 1 + (g * 7) % 20000, g % 100 FROM generate_series(1, 50000) g;
CREATE TABLE sp_fact_wide (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO sp_fact_wide SELECT id, k * 100, v FROM sp_fact_dense;
SET client_min_messages = warning;
CREATE INDEX sp_dense_col ON sp_dense USING sift_col (id, a);
CREATE INDEX sp_wide_col ON sp_wide USING sift_col (id, a);
CREATE INDEX sp_fact_dense_col ON sp_fact_dense USING sift_col (id, k, v);
CREATE INDEX sp_fact_wide_col ON sp_fact_wide USING sift_col (id, k, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) sp_dense, sp_wide, sp_fact_dense, sp_fact_wide;

CREATE FUNCTION scan_cost(kind text) RETURNS float8 LANGUAGE sql AS $$
	SELECT scan_cost FROM sift.explain_why(format($q$SELECT sum(f.v) FILTER (WHERE p.a = 1), sum(f.v) FILTER (WHERE p.a < 3) FROM sp_fact_%s f JOIN sp_%s p ON p.id = f.k$q$, kind, kind))
$$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT scan_cost('wide') > 1.3 * scan_cost('dense') AS wide_span_costs_more;
SELECT k, answer(format($q$SELECT p.a, sum(f.v) FROM sp_fact_%s f JOIN sp_%s p ON p.id = f.k GROUP BY 1$q$, k, k), true) =
          answer(format($q$SELECT p.a, sum(f.v) FROM sp_fact_%s f JOIN sp_%s p ON p.id = f.k GROUP BY 1$q$, k, k), false) AS same_answers
FROM (VALUES ('dense'), ('wide')) v (k) ORDER BY k;

DROP FUNCTION scan_cost(text), answer(text, bool);
DROP TABLE sp_fact_dense, sp_fact_wide, sp_dense, sp_wide;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s8 CASCADE;
RESET client_min_messages;
-- <<< agg_join_span_price

-- >>> agg_join_sparse_key_price
CREATE SCHEMA s9;
GRANT USAGE ON SCHEMA s9 TO PUBLIC;
SET search_path = s9, public;
-- A parent table whose tests have a few outcomes is kept as classes of its keys: an array over the key values, so the keys must fall within the lookup budget. A table whose
-- keys are spread over a range larger than the budget is read as rows and probed in a hash table, which costs a probe for each tuple (a cache miss each, past the cache): the
-- planner prices the classes only for a table that can be one. The answers are those of the normal plan.
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET work_mem = '128kB';

CREATE TABLE par_dense (id int PRIMARY KEY, a int) WITH (autovacuum_enabled = false);
INSERT INTO par_dense SELECT g, g % 7 FROM generate_series(1, 5000) g;
CREATE TABLE par_sparse (id int PRIMARY KEY, a int) WITH (autovacuum_enabled = false);
INSERT INTO par_sparse SELECT g * 10000, g % 7 FROM generate_series(1, 5000) g;
CREATE TABLE fact_dense (id int, par_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO fact_dense SELECT g, 1 + (g * 7) % 5000, g % 100 FROM generate_series(1, 50000) g;
CREATE TABLE fact_sparse (id int, par_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO fact_sparse SELECT id, par_id * 10000, v FROM fact_dense;
SET client_min_messages = warning;
CREATE INDEX par_dense_col ON par_dense USING sift_col (id, a);
CREATE INDEX par_sparse_col ON par_sparse USING sift_col (id, a);
CREATE INDEX fact_dense_col ON fact_dense USING sift_col (id, par_id, v);
CREATE INDEX fact_sparse_col ON fact_sparse USING sift_col (id, par_id, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) par_dense, par_sparse, fact_dense, fact_sparse;

CREATE FUNCTION scan_cost(kind text) RETURNS float8 LANGUAGE sql AS $$
	SELECT scan_cost FROM sift.explain_why(format($q$SELECT sum(f.v) FILTER (WHERE p.a = 1), sum(f.v) FILTER (WHERE p.a < 3) FROM fact_%s f JOIN par_%s p ON p.id = f.par_id$q$, kind, kind))
$$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT scan_cost('sparse') > 1.3 * scan_cost('dense') AS sparse_keys_cost_more;
SELECT answer(format($q$SELECT p.a, sum(f.v) FROM fact_%s f JOIN par_%s p ON p.id = f.par_id GROUP BY 1$q$, k, k), true) =
       answer(format($q$SELECT p.a, sum(f.v) FROM fact_%s f JOIN par_%s p ON p.id = f.par_id GROUP BY 1$q$, k, k), false) AS same_answers, k
FROM (VALUES ('dense'), ('sparse')) v (k) ORDER BY k;

DROP FUNCTION scan_cost(text), answer(text, bool);
DROP TABLE fact_dense, fact_sparse, par_dense, par_sparse;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s9 CASCADE;
RESET client_min_messages;
-- <<< agg_join_sparse_key_price

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
