-- ORDER BY .. LIMIT, window functions over aggregates, pushed aggregates and the limits of a sift_table (the rows W04 W08 W10 W11 W12 W13 W16 W17 W18 W19 W20 L03 L09 of docs/COVERAGE.md): every
-- statement answers the rows the heap answers with sift.enable_agg = off, on a sift_col index, on a sift_table and over the partitions of a table; plan_ok prints t when it ran as SiftAggV6.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.test_create_format1 = off;
SET sift.agg_force = on;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
DO $$ BEGIN CREATE COLLATION w_nd (provider = icu, locale = 'und-u-ks-level1', deterministic = false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE w_tenants (id int PRIMARY KEY, name text, kind text);
INSERT INTO w_tenants SELECT g, 'tenant ' || g, (ARRAY['outlet', 'popup', 'hq'])[1 + g % 3] FROM generate_series(1, 20) g;

CREATE TABLE wh (id int PRIMARY KEY, tenant_id int, customer_id int, status text, total numeric(12,2), placed date, ts timestamp, deleted_at timestamp, tz timestamptz,
				 n38 numeric(38,10), i4 int, bp char(6), nm name, cd cidr, memo text, nd text COLLATE w_nd) WITH (autovacuum_enabled = false);
INSERT INTO wh
SELECT g, 1 + g % 20, CASE WHEN g % 9 = 0 THEN NULL ELSE g % 700 END, (ARRAY['paid', 'open', 'void'])[1 + g % 3], CASE WHEN g % 11 = 0 THEN NULL ELSE ((g * 37) % 1000) / 4.0 END,
	   date '2024-01-01' + g % 700, timestamp '2024-01-01' + (g % 5000) * interval '1 hour', CASE WHEN g % 4 = 0 THEN timestamp '2025-01-01' + (g % 300) * interval '1 day' END,
	   timestamptz '2024-01-01' + g * interval '7 minutes', (g % 97)::numeric * 1000000000000000000000.5 + g, g % 50, (ARRAY['ab', 'b ', 'a', 'abc  ', 'ab '])[1 + g % 5],
	   (ARRAY['alpha', 'beta', 'gamma', 'delta'])[1 + g % 4]::name, ('10.' || g % 7 || '.0.0/16')::cidr, (ARRAY['cable', 'wire', 'cable wire', 'fiber', 'plug'])[1 + g % 5],
	   (ARRAY['Apple', 'apple', 'Banana', 'banana', 'cherry'])[1 + g % 5]
FROM generate_series(1, 3000) g;
CREATE TABLE wc (LIKE wh INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE wt (LIKE wh INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO wc SELECT * FROM wh;
INSERT INTO wt SELECT * FROM wh;
CREATE INDEX wc_col ON wc USING sift_col (id, tenant_id, customer_id, status, total, placed, ts, deleted_at, tz, n38, i4, bp, nm, cd, nd);
CREATE INDEX wc_fts ON wc USING sift (memo);
SELECT sift.table_maintain('wt') IS NOT NULL AS maintained;

CREATE TABLE wph (LIKE wh INCLUDING DEFAULTS) PARTITION BY RANGE (placed);
CREATE TABLE wph_a PARTITION OF wph FOR VALUES FROM ('2024-01-01') TO ('2024-09-01');
CREATE TABLE wph_b PARTITION OF wph FOR VALUES FROM ('2024-09-01') TO ('2025-06-01');
CREATE TABLE wph_c PARTITION OF wph DEFAULT;
INSERT INTO wph SELECT * FROM wh;
CREATE TABLE wp (LIKE wh INCLUDING DEFAULTS) PARTITION BY RANGE (placed);
CREATE TABLE wp_a PARTITION OF wp FOR VALUES FROM ('2024-01-01') TO ('2024-09-01');
CREATE TABLE wp_b PARTITION OF wp FOR VALUES FROM ('2024-09-01') TO ('2025-06-01');
CREATE TABLE wp_c PARTITION OF wp DEFAULT;
INSERT INTO wp SELECT * FROM wh;
CREATE INDEX wp_col ON wp USING sift_col (id, tenant_id, customer_id, status, total, placed, ts);

CREATE FUNCTION rows_of(q text, agg boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n''), '''') FROM (%s) r', q) INTO r;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN r;
END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT rows_of(q, true) = rows_of(q, false) $$;

CREATE FUNCTION plan_ok(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	RETURN n = 1;
END $$;

-- W04: a window function over the aggregate of its own query level
SELECT parity('SELECT status, count(*), rank() OVER (ORDER BY count(*) DESC) FROM wc GROUP BY status ORDER BY status') AS w04_index;
SELECT parity('SELECT status, count(*), sum(count(*)) OVER (ORDER BY status) FROM wt GROUP BY status ORDER BY status') AS w04_table;
SELECT plan_ok('SELECT status, count(*), rank() OVER (ORDER BY count(*) DESC) FROM wc GROUP BY status') AS w04_plan;

-- W08: sort keys of several types, NULLS FIRST or LAST, OFFSET
SELECT parity('SELECT id, deleted_at, total FROM wc ORDER BY deleted_at DESC NULLS LAST, total, id LIMIT 7 OFFSET 3') AS w08_index;
SELECT parity('SELECT id, deleted_at, total FROM wt ORDER BY deleted_at DESC NULLS LAST, total, id LIMIT 7 OFFSET 3') AS w08_table;
SELECT parity('SELECT id FROM wc ORDER BY tz DESC, placed NULLS FIRST, id LIMIT 9') AS w08_tz;
SELECT parity('SELECT id FROM wc ORDER BY status DESC, total NULLS FIRST, id LIMIT 9') AS w08_text;
SELECT plan_ok('SELECT id, deleted_at, total FROM wc ORDER BY deleted_at DESC NULLS LAST, total, id LIMIT 7 OFFSET 3') AS w08_plan;

-- W10: a stable function, a numeric leaf of 38 digits, an interval, an array, a jsonb, a cidr and a char(n) as the result
SELECT parity('SELECT id FROM wc ORDER BY now() - ts, id LIMIT 5') AS w10_now;
SELECT parity('SELECT id FROM wc ORDER BY concat(status, tenant_id), id LIMIT 5') AS w10_concat;
SELECT parity('SELECT id FROM wc ORDER BY placed::text COLLATE "C", id LIMIT 5') AS w10_text_of_date;
SELECT parity('SELECT id FROM wc ORDER BY n38 * 2, id LIMIT 12') AS w10_wide_numeric;
SELECT parity('SELECT id FROM wt ORDER BY n38 * 2, id LIMIT 12') AS w10_wide_numeric_table;
SELECT parity('SELECT id FROM wc ORDER BY tz - interval ''3 hours'' DESC NULLS LAST, id LIMIT 12') AS w10_interval_arithmetic;
SELECT parity('SELECT id FROM wc ORDER BY ARRAY[i4, 1], id LIMIT 12') AS w10_array;
SELECT parity('SELECT id FROM wc ORDER BY to_jsonb(i4), id LIMIT 12') AS w10_jsonb;
SELECT parity('SELECT id FROM wc WHERE ts < ''infinity'' ORDER BY ts - TIMESTAMP ''2024-02-01'', id LIMIT 12') AS w10_infinity_bound;
SELECT parity('SELECT id FROM wc ORDER BY total + (random() * 0), id LIMIT 5') AS w10_volatile_keeps_normal_plan;

-- W11: the rows of a join to a table or a sub-select
SELECT parity('SELECT o.id, s.name FROM wc o JOIN w_tenants s ON s.id = o.tenant_id WHERE s.kind = ''outlet'' ORDER BY o.total, o.id LIMIT 5') AS w11_join;
SELECT parity('SELECT id FROM wc WHERE tenant_id IN (SELECT id FROM w_tenants WHERE kind = ''popup'') ORDER BY total, id LIMIT 5') AS w11_semi;
SELECT parity('SELECT id FROM wc o WHERE EXISTS (SELECT 1 FROM w_tenants s WHERE s.id = o.tenant_id AND s.kind = ''hq'') ORDER BY total, id LIMIT 5') AS w11_exists;
SELECT parity('SELECT id FROM wc WHERE tenant_id NOT IN (SELECT id FROM w_tenants WHERE kind = ''hq'') ORDER BY total, id LIMIT 5') AS w11_anti;

-- W12: an OR of searches of one index
SELECT parity('SELECT id FROM wc WHERE memo @@> ''cable'' OR memo @@> ''wire'' ORDER BY placed DESC, id LIMIT 10') AS w12_two_arms;
SELECT parity('SELECT id FROM wc WHERE (memo @@> ''cable'' OR memo @@> ''fiber'') AND status = ''paid'' ORDER BY placed DESC, id LIMIT 10') AS w12_filter;

-- W13: a sift_table
SELECT parity('SELECT id FROM wt ORDER BY i4, id LIMIT 5') AS w13_table;
SELECT plan_ok('SELECT id FROM wt ORDER BY i4, id LIMIT 5') AS w13_plan;

-- W16: a nondeterministic collation, a char(n), a name and a cidr
SELECT parity('SELECT id FROM wc ORDER BY nd, id LIMIT 12 OFFSET 1') AS w16_nondeterministic;
SELECT parity('SELECT id FROM wc ORDER BY nd COLLATE w_nd DESC, id LIMIT 12') AS w16_nondeterministic_desc;
SELECT parity('SELECT id FROM wc ORDER BY bp, id LIMIT 12 OFFSET 1') AS w16_bpchar;
SELECT parity('SELECT id FROM wc ORDER BY nm DESC NULLS FIRST, id LIMIT 12 OFFSET 1') AS w16_name;
SELECT parity('SELECT id FROM wc ORDER BY cd, id LIMIT 12 OFFSET 1') AS w16_cidr;
SELECT parity('SELECT id FROM wt ORDER BY bp, id LIMIT 12 OFFSET 1') AS w16_bpchar_table;

-- W17: the partitions left after pruning, the ties of the first column, the pruned one
SELECT parity('SELECT id, total FROM wp ORDER BY total DESC, id LIMIT 5') AS w17_partitions;
SELECT parity('SELECT id, tenant_id, customer_id FROM wp ORDER BY tenant_id, customer_id NULLS FIRST, id LIMIT 20') AS w17_ties_on_the_first;
SELECT parity('SELECT id, total FROM wp WHERE placed >= DATE ''2024-09-01'' ORDER BY total DESC NULLS LAST, id LIMIT 5') AS w17_pruned;
SELECT plan_ok('SELECT id, total FROM wp ORDER BY total DESC, id LIMIT 5') AS w17_plan;

-- W18, W20: coalesce and an expression of the columns
SELECT parity('SELECT id, customer_id FROM wc ORDER BY coalesce(customer_id, 500) DESC, id LIMIT 7') AS w18_coalesce;
SELECT parity('SELECT id FROM wc ORDER BY lower(status), id LIMIT 7') AS w18_lower;
SELECT plan_ok('SELECT id, customer_id FROM wc ORDER BY coalesce(customer_id, 500) DESC, id LIMIT 7') AS w18_plan;
SELECT parity('SELECT id, customer_id FROM wc ORDER BY customer_id + 1 DESC, id LIMIT 7') AS w20_arithmetic;
SELECT parity('SELECT id FROM wt ORDER BY CASE WHEN total > 100 THEN total ELSE -total END, id LIMIT 7') AS w20_case_table;
SELECT plan_ok('SELECT id, customer_id FROM wc ORDER BY customer_id + 1 DESC, id LIMIT 7') AS w20_plan;

-- W19: the total of the whole set next to the page of the Top-N
SELECT parity('SELECT id, total, count(*) OVER (), sum(total) OVER () FROM wc WHERE status = ''paid'' ORDER BY total, id LIMIT 7') AS w19_index;
SELECT parity('SELECT id, total, count(*) OVER () FROM wt WHERE status = ''paid'' ORDER BY total, id LIMIT 7') AS w19_table;
SELECT parity('SELECT id, total, count(*) OVER () FROM wc WHERE status = ''paid'' ORDER BY total, id LIMIT 7 OFFSET 5000') AS w19_offset_past_the_end;

-- L03: 1600 columns, not 1601
DO $$ BEGIN
	EXECUTE 'CREATE TEMP TABLE w_1600 (' || (SELECT string_agg('c' || g || ' int', ',') FROM generate_series(1, 1600) g) || ') USING sift_table';
END $$;
DO $$ BEGIN
	EXECUTE 'CREATE TEMP TABLE w_1601 (' || (SELECT string_agg('c' || g || ' int', ',') FROM generate_series(1, 1601) g) || ') USING sift_table';
EXCEPTION WHEN too_many_columns THEN RAISE NOTICE 'too_many_columns';
END $$;
SELECT count(*) = 1600 AS l03_1600_columns FROM pg_attribute WHERE attrelid = 'w_1600'::regclass AND attnum > 0;
SELECT to_regclass('w_1601') IS NULL AS l03_1601_refused;

-- L09: the system columns of a sift_table
CREATE TABLE w_sys (id int, v int) USING sift_table;
BEGIN;
INSERT INTO w_sys SELECT i, i FROM generate_series(1, 3) i;
CREATE TABLE w_sys_x AS SELECT pg_current_xact_id()::xid AS x;
COMMIT;
SELECT bool_and(xmin = (SELECT x FROM w_sys_x)) AND bool_and(cmin::text = '0') AND count(*) FILTER (WHERE xmax::text <> '0') = 0 AS l09_system_columns FROM w_sys;

DROP TABLE wh, wc, wt, wph, wp, w_tenants, w_1600, w_sys, w_sys_x;
DROP FUNCTION rows_of(text, boolean), parity(text), plan_ok(text);
DROP COLLATION w_nd;
DROP EXTENSION pg_sift;
