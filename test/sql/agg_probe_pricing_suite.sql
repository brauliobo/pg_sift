-- The joins and probes of an aggregate scan. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;

-- >>> agg_probe_anti_price
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- A correlated child that the scan probes (NOT EXISTS, EXISTS or a scalar sub-select that the planner hook made a lateral probe, `sift_probe`) runs for every tuple that reaches the join, the
-- ones a condition on what the child gives (`IS NULL` of an anti join) drops after the child ran too: the probes are priced by the tuples the scan reads, not by the rows it aggregates.
-- Priced by the rows of the result they cost a hundredth of what they take and beat the load of the child's rows into a lookup table, which is the cheaper plan when most rows of the
-- outer table go through a probe. `lookup` says the plan loads the child's rows (a `Lookup Joins:` line without `sift_probe`), `rows` that the rows are those of sift off.
SET work_mem = '256MB';
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ow (id int PRIMARY KEY, customer_id int NOT NULL, created_at timestamptz NOT NULL, status text NOT NULL) WITH (autovacuum_enabled = false);
CREATE TABLE ow_items (id int PRIMARY KEY, order_id int NOT NULL, qty int NOT NULL, price float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ow SELECT i, 1 + i % 997, timestamptz '2025-01-01' + i * interval '7 minutes', (ARRAY['new','paid','shipped'])[1 + i % 3] FROM generate_series(1, 60000) i;
INSERT INTO ow_items SELECT i, 1 + (i * 7) % 75000, 1 + i % 4, ((i * 37) % 20000) / 100.0 FROM generate_series(1, 150000) i;
CREATE INDEX ow_items_order ON ow_items (order_id);
CREATE INDEX ow_col ON ow USING sift_col (id, customer_id, created_at, status);
CREATE INDEX ow_items_col ON ow_items USING sift_col (id, order_id, qty, price);
VACUUM (FREEZE, ANALYZE) ow, ow_items;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('not exists with a filter',  $q$SELECT count(*) FROM ow o WHERE NOT EXISTS (SELECT 1 FROM ow_items i WHERE i.order_id = o.id AND i.price > 150)$q$),
  ('not exists, grouped',       $q$SELECT o.status, count(*) FROM ow o WHERE NOT EXISTS (SELECT 1 FROM ow_items i WHERE i.order_id = o.id AND i.qty = 4) GROUP BY 1$q$),
  ('exists with a filter',      $q$SELECT count(*) FROM ow o WHERE EXISTS (SELECT 1 FROM ow_items i WHERE i.order_id = o.id AND i.price > 150)$q$);
CREATE FUNCTION probe_check(q text) RETURNS TABLE (engaged bool, lookup bool, probes bool, rows_same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  probes := plan LIKE '%sift_probe%';
  lookup := plan LIKE '%Lookup Joins:%' AND NOT probes;
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  rows_same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT q.name, c.* FROM q, LATERAL probe_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION probe_check(text);
DROP TABLE q, ow, ow_items;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_probe_anti_price

-- >>> agg_probe_heap_price
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- The direct probe of a correlated child (an index scan of the child's btree for each parent) is priced for the heap row each entry fetches: the page visit of a
-- table not in the order of the key, and the condition of the child that the btree does not hold, tested on the fetched row. The probe of a parent's children of
-- ten thousand parents and half a million rows costs more than the table of the children read from the leaves of their sift_col index, which the scan takes.
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET work_mem = '256MB';

CREATE TABLE hp_parent (id int PRIMARY KEY, grp int) WITH (autovacuum_enabled = false);
INSERT INTO hp_parent SELECT g, g % 40 FROM generate_series(1, 50000) g;
CREATE TABLE hp_child (id bigint PRIMARY KEY, parent_id int, amount numeric(12, 2), at timestamp) WITH (autovacuum_enabled = false);
INSERT INTO hp_child SELECT g, 1 + (g::bigint * 7919) % 45000, (g % 1000)::numeric, timestamp '2024-01-01' + (g % 365) * interval '1 day'
FROM generate_series(1, 200000) g;
CREATE INDEX hp_child_idx ON hp_child (parent_id, at);
CREATE INDEX hp_parent_col ON hp_parent USING sift_col (id, grp);
CREATE INDEX hp_child_col ON hp_child USING sift_col (parent_id, amount, at);
VACUUM (FREEZE, ANALYZE) hp_parent, hp_child;

CREATE FUNCTION plan_joins(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'none'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins:' THEN r := trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT plan_joins($q$SELECT p.grp, count(*) FROM hp_parent p
 WHERE EXISTS (SELECT 1 FROM hp_child c WHERE c.parent_id = p.id AND c.amount > 990) GROUP BY p.grp$q$) AS joins;

DROP FUNCTION plan_joins(text);
DROP TABLE hp_parent, hp_child;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_probe_heap_price

-- >>> agg_probe_lookup_explain
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- The Lookup line of EXPLAIN ANALYZE says what the direct probes of a correlated child did (the keys probed, the leaf pages they moved to, the pages above the leaves they pinned, the heap
-- fetches and the entries of an index-only child that the visibility map answered) and how much of the lookup tables' pages the visibility map holds all-visible; under half of them (a table of
-- 64 pages or more) it adds a hint to VACUUM. The numbers move with the host, so the cases state what must hold of them, and one text line is shown with its digits replaced.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 100; SET random_page_cost = 4;
SET sift.agg_force = on;

CREATE TABLE dr (id int, n int);
INSERT INTO dr SELECT g, (g * 37) % 30000 FROM generate_series(1, 400) g;
CREATE INDEX dr_col ON dr USING sift_col (id, n);
VACUUM (FREEZE, ANALYZE) dr;
-- lookup tables of about five hundred pages: one read through the heap, one through a covering index; neither is vacuumed yet
CREATE TABLE lk_heap (k int, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO lk_heap SELECT g, g % 7, repeat('x', 100) FROM generate_series(0, 29999) g;
CREATE INDEX lk_heap_k ON lk_heap (k);
CREATE TABLE lk_cover (k int, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO lk_cover SELECT g, g % 7, repeat('x', 100) FROM generate_series(0, 29999) g;
CREATE INDEX lk_cover_k ON lk_cover (k) INCLUDE (v);

CREATE FUNCTION lookup_node(tab text, fmt text DEFAULT 'JSON') RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT %s) SELECT x.v, count(*) FROM dr d LEFT JOIN LATERAL (SELECT v FROM %I WHERE k = d.n LIMIT 1) x ON true GROUP BY 1', fmt, tab) LOOP
    RETURN NEXT l;
  END LOOP;
END $$;
-- the node of the scan, as jsonb
CREATE FUNCTION lookup_props(tab text) RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_path_query_first(string_agg(l, '')::jsonb, '$[0].Plan.** ? (exists(@."Child Direct Probes"))') FROM lookup_node(tab) l $$;
CREATE FUNCTION shape(tab text) RETURNS TABLE (probed bool, leaves_bounded bool, pins_counted bool, heap_fetches text, index_only_entries text, all_visible text, hint bool) LANGUAGE sql AS $$
  SELECT (n->>'Child Direct Probes')::int > 0,
         (n->>'Child Probe Leaves')::int BETWEEN 1 AND (n->>'Child Direct Probes')::int,
         (n->>'Child Probe Upper Pages Pinned')::int > 0,
         CASE WHEN (n->>'Child Probe Heap Fetches')::int = (n->>'Child Direct Probes')::int THEN 'one a probe' WHEN (n->>'Child Probe Heap Fetches')::int = 0 THEN 'none' ELSE 'some' END,
         CASE WHEN (n->>'Child Probe Index Only Entries')::int = (n->>'Child Direct Probes')::int THEN 'one a probe' WHEN n ? 'Child Probe Index Only Entries' THEN 'some' ELSE 'none' END,
         (n->>'Lookup Pages All Visible') || ' %',
         n ? 'Lookup Hint'
  FROM (SELECT lookup_props(tab) AS n) q $$;

-- not vacuumed: nothing is all-visible, so the hint comes; the heap table is read through the heap, the covering one too (no page is all-visible)
SELECT 'heap table' AS lookup, * FROM shape('lk_heap');
SELECT 'covering index' AS lookup, * FROM shape('lk_cover');
-- vacuumed: the hint is gone; the covering index answers from the visibility map
VACUUM (FREEZE, ANALYZE) lk_heap, lk_cover;
SELECT 'heap table' AS lookup, * FROM shape('lk_heap');
SELECT 'covering index' AS lookup, * FROM shape('lk_cover');
-- the text line, its digits replaced
SELECT regexp_replace(regexp_replace(trim(l), ', [0-9]+ child blocks prefetched', ''), '[0-9]+', 'N', 'g') AS lookup_line FROM lookup_node('lk_cover', 'TEXT') l WHERE l ~ '^ *Lookup: ';
SELECT regexp_replace(regexp_replace(trim(l), ', [0-9]+ child blocks prefetched', ''), '[0-9]+', 'N', 'g') AS lookup_line FROM lookup_node('lk_heap', 'TEXT') l WHERE l ~ '^ *Lookup: ';

DROP FUNCTION shape(text), lookup_props(text), lookup_node(text, text);
DROP TABLE dr, lk_heap, lk_cover;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_probe_lookup_explain

-- >>> agg_probe_range_pricing
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- An IN over a tenant of 718 of 20,000 ids, with a range on the ids (`dim_a_id <= k`), is read as the set of the tenant's ids once the range holds more than about a hundred of them: the
-- plan that probes the pair table for each key the scan meets resolves each fact row against its key's answer (66 instructions a row, 35 of them in the join resolution, against 7 for a row
-- tested in the set's byte table), which the price of a probe did not count, so the probes were kept up to 5,000 keys where the set takes 88.6 M instructions against 167.3 M over 2,000,000
-- facts (the two take the same up to 100 keys, 82.6 M, and the probes 103.1 and 113.6 M against 87.1 and 87.4 M at 500 and 1,000).
SET work_mem = '256MB';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SELECT setseed(0.42);
CREATE TABLE facts (id bigserial PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts (dim_a_id, created_at, kind, amount) SELECT 1 + (20000 * random() ^ 1.8)::int % 20000, timestamptz '2026-01-01' + random() * interval '365 days', (random() * 9)::int,
	(random() * 1000)::numeric(8,2) FROM generate_series(1, 200000) g;
CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id)) WITH (autovacuum_enabled = false);
INSERT INTO scope_pairs SELECT id, 1 FROM generate_series(1, 20000) id WHERE id % 28 = 3;
INSERT INTO scope_pairs SELECT id, 2 FROM generate_series(1, 18000) id;
INSERT INTO scope_pairs SELECT id, 3 + id % 40 FROM generate_series(1, 20000) id;
CREATE INDEX facts_dim_a ON facts (dim_a_id);
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, amount);
VACUUM (FREEZE, ANALYZE) facts, scope_pairs;

-- the lookup join of a semi join over a unique key is an inner join from PostgreSQL 19: the label's (semi) is not part of what the line proves
CREATE FUNCTION plan_joins(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'none'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins:' THEN r := trim(replace(l, ' (semi)', '')); END IF;
	END LOOP;
	RETURN r;
END $$;
-- the rows of the statement with the scan and without it, as one md5 each
CREATE FUNCTION answers(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (%s) t', sql) INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (%s) t', sql) INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;
CREATE FUNCTION statement(k int) RETURNS text LANGUAGE sql AS
	$$ SELECT format('SELECT kind, count(*), sum(amount) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM scope_pairs WHERE tenant_id = 1) AND dim_a_id <= %s GROUP BY kind', k) $$;
SELECT k, plan_joins(statement(k)) AS joins, answers(statement(k)) AS answers FROM unnest(ARRAY[300, 1000, 5000, 20000]) k;

DROP FUNCTION plan_joins(text), answers(text), statement(int);
DROP TABLE facts, scope_pairs;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_probe_range_pricing

-- >>> agg_probe_run_crossover
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- A correlated LIMIT 1 child is probed in its btree for each distinct key of the scan (a run: 6,000 to 8,000 instructions, 12,000 to 13,000 under a collation other than C), or the
-- table it reads is loaded whole (1,000 instructions a row) and the keys looked up in it. The probes win up to about a seventh of the rows of the table as keys
-- (a twelfth under an ICU collation); the statements pin both sides for an integer key and a text key under an ICU collation, and the one number of keys between the two
-- crossovers where the two keys choose differently (calibration table in docs/AGG.md).
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;

CREATE TABLE ev (id int, fk int, fku text COLLATE "und-x-icu") WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i, 'k' || i FROM generate_series(1, 20000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, fk, fku);
CREATE TABLE dm_i (k int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dm_i SELECT i, 'l' || i % 11 FROM generate_series(1, 20000) i;
CREATE TABLE dm_u (k text COLLATE "und-x-icu" PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dm_u SELECT 'k' || i, 'l' || i % 11 FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) ev, dm_i, dm_u;

-- how the scan reads the lookup table for the keys 1 to n of the facts: probes of its btree, or the table whole
CREATE FUNCTION lookup_form(tab text, col text, n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'none';
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT d.label, count(*) FROM ev LEFT JOIN LATERAL (SELECT label FROM %s x WHERE x.k = ev.%s LIMIT 1) d ON true WHERE ev.fk <= %s GROUP BY 1', tab, col, n) LOOP
		IF l ~ 'child direct probes' THEN r := 'probes'; ELSIF l ~ 'first row tables' THEN r := 'whole'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT n, lookup_form('dm_i', 'fk', n) AS integer_key, lookup_form('dm_u', 'fku', n) AS icu_key
FROM unnest(ARRAY[200, 1000, 2100, 6000, 12000]) n;

DROP FUNCTION lookup_form(text, text, int);
DROP TABLE ev, dm_i, dm_u;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_probe_run_crossover

DROP EXTENSION pg_sift;
