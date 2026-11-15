\set ECHO none
-- Shared by the agg_having_* tests (\ir test/agg_having_helpers.sql): one fixture and the helpers that run a statement with the aggregate scan on and off.
--
-- Fixture: hv_events (30,000 rows, a sift_col index) with skewed and tied groups, NULL keys and NULL measures, duplicate-heavy columns, a text, a date and a numeric key;
-- hv_tenants (a lookup with a NULL row) and hv_empty (no row). tenant_id 1 holds 40% of the rows; 6 and 13 tie at 990 rows, 8, 9, 11 are within one row of each other;
-- 15 has 5 rows and only NULL measures (its sum is NULL); 16 has one row; 17 has three equal rows (max = min); the NULL tenant has 186 rows.
--
-- having_run(q, ordered) runs q with the scan on (the table scans off, so a Custom Scan (SiftAgg) runs when the statement is one it can) and off, and says
--   served            the plan has the scan node
--   verdict           'sift', or the arm:slug of every arm sift.explain_why refuses
--   where_in_scan     the conditions of the rows (WHERE) are tested by the scan before the rows are grouped (a `Filters:` line of the node)
--   having_on_groups  the node tests a condition on the groups after they are aggregated (a `Filter:` line of the node)
--   rows_same         the rows of the two runs are the same (sorted, or in the order they come when `ordered`); floats at extra_float_digits = -5, as a float sum is added in another order
--   n                 how many rows q returns (so no statement passes by returning nothing in both)
-- having_same(a, b, expect) sets a with the scan on against b with the scan on (a_vs_b: same or differs; as_expected: it is what `expect` says, 'same' unless the pair is
--   one that differs by design) and against a with the scan off (a_same_off).
CREATE TABLE hv_tenants (id int PRIMARY KEY, label text, tier smallint, active bool);
CREATE TABLE hv_events (id int PRIMARY KEY, tenant_id int, category_id int, kind text, event_date date, status smallint, counter_a int, counter_b int,
  value_num numeric(10,2), amount float8, label text, flag_a bool) WITH (autovacuum_enabled = false);
CREATE TABLE hv_empty (id int PRIMARY KEY, tenant_id int, counter_a int) WITH (autovacuum_enabled = false);
INSERT INTO hv_tenants SELECT i, 'tenant_' || i, i % 3, i % 4 <> 0 FROM generate_series(1, 14) i;
INSERT INTO hv_tenants VALUES (20, NULL, NULL, NULL);
INSERT INTO hv_events
SELECT i,
  CASE WHEN i % 10 < 4 THEN 1 WHEN i % 97 = 0 THEN NULL ELSE 2 + (i % 10) + (i % 3) END,
  CASE WHEN i % 61 = 0 THEN NULL ELSE 1 + i % 6 END,
  CASE WHEN i % 53 = 0 THEN NULL ELSE 'k' || (1 + i % 5) END,
  CASE WHEN i % 71 = 0 THEN NULL ELSE DATE '2026-01-01' + (i % 30) END,
  (i % 4)::smallint,
  CASE WHEN i % 17 = 0 THEN NULL ELSE i % 5 END,
  i % 9,
  CASE WHEN i % 13 = 0 THEN NULL ELSE ((i * 37) % 1000) / 10.0 END,
  (i % 90) / 9.0,
  'l' || (i % 40),
  i % 7 = 0
FROM generate_series(1, 30000) i;
INSERT INTO hv_events SELECT 30000 + i, 15, 1, 'k1', DATE '2026-02-01', 0, NULL, NULL, NULL, NULL, 'l0', false FROM generate_series(1, 5) i;
INSERT INTO hv_events VALUES (30006, 16, 2, 'k2', DATE '2026-02-02', 1, 7, 7, 7.00, 7.0, 'l1', true);
INSERT INTO hv_events SELECT 30006 + i, 17, 3, 'k3', DATE '2026-02-03', 2, 4, 4, 4.00, 4.0, 'l2', false FROM generate_series(1, 3) i;
CREATE INDEX hv_events_col ON hv_events USING sift_col (id, tenant_id, category_id, kind, event_date, status, counter_a, counter_b, value_num, amount, label, flag_a);
CREATE INDEX hv_empty_col ON hv_empty USING sift_col (id, tenant_id, counter_a);
VACUUM (FREEZE, ANALYZE) hv_tenants, hv_events, hv_empty;

CREATE FUNCTION having_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; plan text := '';
BEGIN
  PERFORM set_config('sift.enable_agg', 'on', true);
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  RETURN plan;
END $$;
CREATE FUNCTION having_rows(q text, agg bool, ordered bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  PERFORM set_config('extra_float_digits', '-5', true);
  PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
  PERFORM set_config(s, (NOT agg)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'CREATE TEMP TABLE hv_result ON COMMIT DROP AS ' || q;
  SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY CASE WHEN ordered THEN NULL ELSE t::text END, t.ctid), ''))|| ' ' || count(*) INTO r FROM hv_result t;
  DROP TABLE hv_result;
  RETURN r;
END $$;
CREATE FUNCTION having_run(q text, ordered bool DEFAULT false) RETURNS TABLE (served bool, verdict text, where_in_scan bool, having_on_groups bool, rows_same bool, n bigint)
LANGUAGE plpgsql AS $$
DECLARE plan text; a text;
BEGIN
  plan := having_plan(q);
  served := plan LIKE '%Custom Scan (SiftAgg)%';
  where_in_scan := CASE WHEN served THEN plan ~ E'\n +Filters: ' END;
  having_on_groups := CASE WHEN served THEN plan ~ E'\n +Filter: ' END;
  verdict := CASE WHEN served THEN 'sift' ELSE (SELECT string_agg(arm || ':' || slug, ',' ORDER BY arm) FROM sift.explain_why(q) WHERE NOT used) END;
  a := having_rows(q, true, ordered);
  rows_same := a = having_rows(q, false, ordered);
  n := split_part(a, ' ', 2)::bigint;
  RETURN NEXT;
END $$;
CREATE FUNCTION having_same(a text, b text, expect text DEFAULT 'same') RETURNS TABLE (a_served bool, b_served bool, a_vs_b text, as_expected bool, a_same_off bool, n bigint)
LANGUAGE plpgsql AS $$
DECLARE ra text;
BEGIN
  a_served := having_plan(a) LIKE '%Custom Scan (SiftAgg)%';
  b_served := having_plan(b) LIKE '%Custom Scan (SiftAgg)%';
  ra := having_rows(a, true, false);
  a_vs_b := CASE WHEN ra = having_rows(b, true, false) THEN 'same' ELSE 'differs' END;
  as_expected := a_vs_b = expect;
  a_same_off := ra = having_rows(a, false, false);
  n := split_part(ra, ' ', 2)::bigint;
  RETURN NEXT;
END $$;
\set ECHO all
