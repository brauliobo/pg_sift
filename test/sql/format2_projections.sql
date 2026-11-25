-- Secondary projections of format major 2: a pre-aggregate and a sort order are created, built from the parts a vacuum has sealed, listed, chosen by the aggregate scan only when they
-- answer it exactly (the answer is the one of the heap through every change of the rows), and dropped with their pages.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION p_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

CREATE FUNCTION p_uses(q text, proj text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; found boolean := false;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		found := found OR line LIKE '%Projection: ' || proj || '%';
	END LOOP;
	RETURN found;
END $$;

CREATE TABLE p (id bigint, tenant int, status text, amount bigint);
CREATE INDEX p_idx ON p USING sift_col (id, tenant, status, amount) WITH (key_order = 'id');
-- 5966 rows of 52 bytes fill 38 heap pages exactly, so the rows written after the vacuum land in a page no sealed part has a row in
INSERT INTO p SELECT i, i % 4, (ARRAY['new', 'old', 'mid'])[1 + i % 3], i % 10 FROM generate_series(1, 5966) i;

-- The spec is checked against the columns of the relation
SELECT sift.create_projection('p_idx', 'bad', 'GROUP BY nosuch MEASURES count(*)');
SELECT sift.create_projection('p_idx', 'bad', 'ORDER BY id, id');
SELECT sift.create_projection('p_idx', '', 'ORDER BY id');
SELECT sift.create_projection('p', 'bad', 'ORDER BY id');

SELECT sift.create_projection('p_idx', 'by_tenant', 'group by tenant, status measures count(*), sum(amount), min(amount), max(amount)');
SELECT sift.create_projection('p_idx', 'by_amount', 'order by amount, id include (tenant)');
SELECT sift.create_projection('p_idx', 'by_tenant', 'ORDER BY id');

SELECT name, kind, state, spec FROM sift.secondary_projections ORDER BY name;

-- Nothing is sealed yet, so nothing is read from the projections
SELECT p_uses('SELECT tenant, count(*), sum(amount) FROM p GROUP BY tenant', 'by_tenant');
SELECT p_same('SELECT tenant, count(*), sum(amount), min(amount), max(amount) FROM p GROUP BY tenant');

-- A vacuum seals the parts and refreshes the projections; the aggregate is then read from the pre-aggregate and is the heap's answer
VACUUM p;
SELECT name, rows > 0 AS has_rows FROM sift.secondary_projections ORDER BY name;
SELECT p_uses('SELECT tenant, count(*), sum(amount) FROM p GROUP BY tenant', 'by_tenant');
SELECT p_uses('SELECT status, tenant, min(amount), max(amount) FROM p WHERE tenant >= 2 GROUP BY status, tenant', 'by_tenant');
SELECT p_same('SELECT tenant, count(*), sum(amount), min(amount), max(amount) FROM p GROUP BY tenant');
SELECT p_same('SELECT status, count(*), avg(amount) FROM p WHERE tenant = 1 GROUP BY status');
SELECT p_uses('SELECT tenant, sum(id) FROM p GROUP BY tenant', 'by_tenant');

-- Rows the projection has not seen are never missed: an insert (the slab), a delete and an update are each the heap's answer; the sealed parts are still read from the projection, the slab and
-- every part it does not stand for from the relation
INSERT INTO p SELECT i, i % 4, 'new', 1 FROM generate_series(5967, 6066) i;
SELECT p_uses('SELECT tenant, count(*), sum(amount) FROM p GROUP BY tenant', 'by_tenant');
SELECT p_same('SELECT tenant, count(*), sum(amount) FROM p GROUP BY tenant');
DELETE FROM p WHERE id <= 300;
SELECT p_same('SELECT tenant, count(*), sum(amount) FROM p GROUP BY tenant');
UPDATE p SET amount = amount + 100 WHERE tenant = 3;
SELECT p_same('SELECT tenant, count(*), sum(amount) FROM p GROUP BY tenant');
VACUUM p;
SELECT p_same('SELECT tenant, count(*), sum(amount), min(amount), max(amount) FROM p GROUP BY tenant');
SELECT p_same('SELECT status, count(*) FROM p WHERE tenant < 3 GROUP BY status');

-- An open transaction that changed rows sees its own changes
BEGIN;
DELETE FROM p WHERE tenant = 0;
SELECT p_same('SELECT tenant, count(*) FROM p GROUP BY tenant');
ROLLBACK;
SELECT p_same('SELECT tenant, count(*) FROM p GROUP BY tenant');

-- Another role may list what it reads and may not create or drop
CREATE ROLE p_reader;
GRANT SELECT ON p TO p_reader;
SET ROLE p_reader;
SELECT count(*) FROM sift.secondary_projections;
SELECT sift.create_projection('p_idx', 'nope', 'ORDER BY id');
SELECT sift.drop_projection('p_idx', 'by_tenant');
RESET ROLE;
REVOKE SELECT ON p FROM p_reader;
DROP ROLE p_reader;

-- Dropping takes the definition and the pages
SELECT sift.drop_projection('p_idx', 'by_amount');
SELECT sift.drop_projection('p_idx', 'by_amount');
SELECT sift.drop_projection('p_idx', 'by_tenant');
SELECT count(*) FROM sift.secondary_projections;
SELECT p_uses('SELECT tenant, count(*) FROM p GROUP BY tenant', 'by_tenant');
SELECT count(*) FROM sift.check('p_idx', false, 100);

-- A pre-aggregate answers predicates on text (an equality and an IN list) and integer columns from its cells for the sealed parts and from the relation for the rest; the aggregate plan takes
-- no boolean or uuid qual yet, so those statements run on the relation and p_uses is false for them
-- rows of 68 bytes: 3960 of them fill 33 heap pages exactly, as the 5966 rows of p do
CREATE TABLE w (id bigint, flag boolean, ref uuid, label text, qty int);
CREATE INDEX w_idx ON w USING sift_col (id, flag, ref, label, qty) WITH (key_order = 'id');
INSERT INTO w SELECT i, i % 3 = 0, ('00000000-0000-0000-0000-00000000000' || (i % 4))::uuid, (ARRAY['red', 'grn', 'blu'])[1 + i % 3], i % 7 FROM generate_series(1, 3960) i;
SELECT sift.create_projection('w_idx', 'by_kind', 'GROUP BY flag, ref, label, qty MEASURES count(*), sum(qty)');
VACUUM w;
SELECT p_uses('SELECT label, count(*), sum(qty) FROM w WHERE flag GROUP BY label', 'by_kind');
SELECT p_uses('SELECT flag, count(*) FROM w WHERE label = ''red'' GROUP BY flag', 'by_kind');
SELECT p_uses('SELECT label, count(*) FROM w WHERE label IN (''red'', ''blu'') GROUP BY label', 'by_kind');
SELECT p_uses('SELECT flag, sum(qty) FROM w WHERE ref = ''00000000-0000-0000-0000-000000000002'' GROUP BY flag', 'by_kind');
SELECT p_uses('SELECT label, count(*) FROM w WHERE ref IN (''00000000-0000-0000-0000-000000000001'', ''00000000-0000-0000-0000-000000000003'') GROUP BY label', 'by_kind');
SELECT p_uses('SELECT label, count(*) FROM w WHERE qty >= 3 AND qty < 6 AND NOT flag GROUP BY label', 'by_kind');
SELECT p_same('SELECT label, count(*), sum(qty) FROM w WHERE flag GROUP BY label');
SELECT p_same('SELECT flag, count(*) FROM w WHERE label = ''red'' GROUP BY flag');
SELECT p_same('SELECT label, count(*) FROM w WHERE label IN (''red'', ''blu'') GROUP BY label');
SELECT p_same('SELECT flag, sum(qty) FROM w WHERE ref = ''00000000-0000-0000-0000-000000000002'' GROUP BY flag');
SELECT p_same('SELECT label, count(*) FROM w WHERE ref IN (''00000000-0000-0000-0000-000000000001'', ''00000000-0000-0000-0000-000000000003'') GROUP BY label');
SELECT p_same('SELECT label, count(*) FROM w WHERE qty >= 3 AND qty < 6 AND NOT flag GROUP BY label');
-- Rows written after the vacuum, deleted or changed since: the answer is still the heap's
INSERT INTO w SELECT i, i % 5 = 0, ('00000000-0000-0000-0000-00000000000' || (i % 4))::uuid, (ARRAY['red', 'grn', 'blu'])[1 + i % 3], i % 7 FROM generate_series(3961, 4160) i;
SELECT p_uses('SELECT label, count(*), sum(qty) FROM w WHERE flag GROUP BY label', 'by_kind');
SELECT p_uses('SELECT flag, count(*) FROM w WHERE label = ''red'' GROUP BY flag', 'by_kind');
SELECT p_uses('SELECT label, count(*) FROM w WHERE label IN (''red'', ''blu'') GROUP BY label', 'by_kind');
SELECT p_uses('SELECT flag, sum(qty) FROM w WHERE ref = ''00000000-0000-0000-0000-000000000002'' GROUP BY flag', 'by_kind');
SELECT p_uses('SELECT label, count(*) FROM w WHERE ref IN (''00000000-0000-0000-0000-000000000001'', ''00000000-0000-0000-0000-000000000003'') GROUP BY label', 'by_kind');
SELECT p_uses('SELECT label, count(*) FROM w WHERE qty >= 3 AND qty < 6 AND NOT flag GROUP BY label', 'by_kind');
DELETE FROM w WHERE id <= 150;
UPDATE w SET label = 'red', flag = true WHERE id BETWEEN 1000 AND 1100;
SELECT p_same('SELECT label, count(*), sum(qty) FROM w WHERE flag GROUP BY label');
SELECT p_same('SELECT flag, count(*) FROM w WHERE label = ''red'' GROUP BY flag');
SELECT p_same('SELECT label, count(*) FROM w WHERE label IN (''red'', ''blu'') GROUP BY label');
SELECT p_same('SELECT flag, sum(qty) FROM w WHERE ref = ''00000000-0000-0000-0000-000000000002'' GROUP BY flag');
SELECT p_same('SELECT label, count(*) FROM w WHERE ref IN (''00000000-0000-0000-0000-000000000001'', ''00000000-0000-0000-0000-000000000003'') GROUP BY label');
SELECT p_same('SELECT label, count(*) FROM w WHERE qty >= 3 AND qty < 6 AND NOT flag GROUP BY label');
VACUUM w;
SELECT p_same('SELECT label, count(*), sum(qty) FROM w WHERE flag GROUP BY label');
SELECT p_same('SELECT flag, count(*) FROM w WHERE label = ''red'' GROUP BY flag');
SELECT p_same('SELECT label, count(*) FROM w WHERE label IN (''red'', ''blu'') GROUP BY label');
SELECT p_same('SELECT flag, sum(qty) FROM w WHERE ref = ''00000000-0000-0000-0000-000000000002'' GROUP BY flag');
SELECT p_same('SELECT label, count(*) FROM w WHERE ref IN (''00000000-0000-0000-0000-000000000001'', ''00000000-0000-0000-0000-000000000003'') GROUP BY label');
SELECT p_same('SELECT label, count(*) FROM w WHERE qty >= 3 AND qty < 6 AND NOT flag GROUP BY label');
SELECT sift.drop_projection('w_idx', 'by_kind');
SELECT count(*) FROM sift.check('w_idx', false, 100);

-- A sort projection answers ORDER BY ... LIMIT through SiftProjSortScan, which names it in EXPLAIN: a part with key marks is sought by its marks (the granules outside the range are not read),
-- numeric predicates are decided on the stored scaled integers (a NaN, which the cells cannot judge, by the row itself), and the node is never planned over row level security, a volatile
-- expression, an order the projection does not have, or a join
CREATE FUNCTION p_same_order(q text, ref text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, '',''), '''') FROM (%s) r', q) INTO a;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '',''), '''') FROM (%s) r', ref) INTO b;
	RETURN CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT: ' || a || ' / ' || b END;
END $$;

CREATE FUNCTION p_skipped(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE line text; n bigint := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF line LIKE '%Projection Granules Skipped: %' THEN n := substring(line from '[0-9]+$')::bigint; END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE TABLE s (id bigint, qty int, amount numeric(12,2), label text);
CREATE INDEX s_idx ON s USING sift_col (id, qty, amount, label) WITH (key_order = 'id');
INSERT INTO s SELECT i, i % 997, (i % 500) * 1.25 - 100, 'l' || (i % 9) FROM generate_series(1, 30000) i;
SELECT sift.create_projection('s_idx', 'by_qty', 'ORDER BY qty, id INCLUDE (amount, label)');
VACUUM s;
SELECT p_uses('SELECT id, qty FROM s ORDER BY qty, id LIMIT 20', 'by_qty');
SELECT p_same_order('SELECT id, qty FROM s ORDER BY qty, id LIMIT 20', 'SELECT id, qty FROM s ORDER BY qty + 0, id LIMIT 20');
SELECT p_same_order('SELECT id, qty FROM s ORDER BY qty, id LIMIT 5 OFFSET 7', 'SELECT id, qty FROM s ORDER BY qty + 0, id LIMIT 5 OFFSET 7');
SELECT p_uses('SELECT id FROM s WHERE qty >= 900 ORDER BY qty, id LIMIT 10', 'by_qty');
CREATE TABLE k (id bigint, qty int);
CREATE INDEX k_idx ON k USING sift_col (id, qty) WITH (key_order = 'id');
INSERT INTO k SELECT i, i % 997 FROM generate_series(1, 80000) i;
SELECT sift.create_projection('k_idx', 'by_qty', 'ORDER BY qty, id');
VACUUM k;
SELECT p_skipped('SELECT id FROM k WHERE qty >= 900 ORDER BY qty, id LIMIT 10') > 0;
SELECT p_same_order('SELECT id FROM k WHERE qty >= 900 ORDER BY qty, id LIMIT 10', 'SELECT id FROM k WHERE qty >= 900 ORDER BY qty + 0, id LIMIT 10');
DROP TABLE k;
SELECT p_same_order('SELECT id FROM s WHERE qty >= 900 ORDER BY qty, id LIMIT 10', 'SELECT id FROM s WHERE qty >= 900 ORDER BY qty + 0, id LIMIT 10');
SELECT p_same_order('SELECT id FROM s WHERE qty = 17 ORDER BY qty, id LIMIT 10', 'SELECT id FROM s WHERE qty = 17 ORDER BY qty + 0, id LIMIT 10');
SELECT p_same_order('SELECT id FROM s WHERE qty IN (3, 500, 996) ORDER BY qty, id LIMIT 12', 'SELECT id FROM s WHERE qty IN (3, 500, 996) ORDER BY qty + 0, id LIMIT 12');
-- Rows the projection has not seen (the slab, a delete, an update) and a NaN and a NULL among the numerics
INSERT INTO s VALUES (30001, 3, 'NaN', 'nan'), (30002, 3, NULL, 'null'), (30003, 3, 100.50, 'edge');
DELETE FROM s WHERE id <= 40;
UPDATE s SET amount = amount + 1 WHERE id BETWEEN 5000 AND 5100;
SELECT p_same_order('SELECT id, amount FROM s ORDER BY qty, id LIMIT 30', 'SELECT id, amount FROM s ORDER BY qty + 0, id LIMIT 30');
SELECT p_same_order('SELECT id, amount FROM s WHERE amount > 100.5 ORDER BY qty, id LIMIT 50', 'SELECT id, amount FROM s WHERE amount > 100.5 ORDER BY qty + 0, id LIMIT 50');
SELECT p_same_order('SELECT id, amount FROM s WHERE amount = 100.50 ORDER BY qty, id LIMIT 50', 'SELECT id, amount FROM s WHERE amount = 100.50 ORDER BY qty + 0, id LIMIT 50');
SELECT p_same_order('SELECT id, amount FROM s WHERE amount <= -50 ORDER BY qty, id LIMIT 50', 'SELECT id, amount FROM s WHERE amount <= -50 ORDER BY qty + 0, id LIMIT 50');
SELECT p_same_order('SELECT id, amount FROM s WHERE amount IN (0.00, 12.50, 100.50) ORDER BY qty, id LIMIT 50', 'SELECT id, amount FROM s WHERE amount IN (0.00, 12.50, 100.50) ORDER BY qty + 0, id LIMIT 50');
SELECT p_same_order('SELECT id, label FROM s WHERE label = ''l4'' AND amount >= 0 ORDER BY qty, id LIMIT 40', 'SELECT id, label FROM s WHERE label = ''l4'' AND amount >= 0 ORDER BY qty + 0, id LIMIT 40');
VACUUM s;
SELECT p_uses('SELECT id, qty FROM s ORDER BY qty, id LIMIT 20', 'by_qty');
SELECT p_same_order('SELECT id, amount FROM s WHERE amount > 100.5 ORDER BY qty, id LIMIT 50', 'SELECT id, amount FROM s WHERE amount > 100.5 ORDER BY qty + 0, id LIMIT 50');
-- A rescanned node (the inner side of a lateral join) gives the same rows each time
SELECT p_same_order('SELECT g, x.id FROM generate_series(1, 3) g, LATERAL (SELECT id FROM s ORDER BY qty, id LIMIT 2) x ORDER BY g, x.id', 'SELECT g, x.id FROM generate_series(1, 3) g, LATERAL (SELECT id FROM s ORDER BY qty + 0, id LIMIT 2) x ORDER BY g, x.id');
-- Never planned: an order the projection does not have, a volatile qual, a predicate it cannot test, no limit, and row level security
SELECT p_uses('SELECT id, qty FROM s ORDER BY qty DESC, id LIMIT 20', 'by_qty');
SELECT p_uses('SELECT id FROM s WHERE qty >= random() * 900 ORDER BY qty, id LIMIT 10', 'by_qty');
SELECT p_uses('SELECT id FROM s WHERE label LIKE ''l1%'' ORDER BY qty, id LIMIT 10', 'by_qty');
SELECT p_uses('SELECT id FROM s ORDER BY qty, id', 'by_qty');
ALTER TABLE s ENABLE ROW LEVEL SECURITY;
CREATE POLICY s_pol ON s USING (qty < 500);
CREATE ROLE s_user;
GRANT SELECT ON s TO s_user;
SET ROLE s_user;
SELECT p_uses('SELECT id, qty FROM s ORDER BY qty, id LIMIT 20', 'by_qty');
SELECT p_same_order('SELECT id, qty FROM s ORDER BY qty, id LIMIT 20', 'SELECT id, qty FROM s ORDER BY qty + 0, id LIMIT 20');
RESET ROLE;
DROP OWNED BY s_user;
DROP ROLE s_user;
SELECT sift.drop_projection('s_idx', 'by_qty');
SELECT p_uses('SELECT id, qty FROM s ORDER BY qty, id LIMIT 20', 'by_qty');
SELECT count(*) FROM sift.check('s_idx', false, 100);
-- A sift_table of Format 2.0 takes a projection as an index does, while it is empty and once it has parts; one of Format 1 is refused
CREATE TABLE tp (id int, d date, v int) USING sift_table;
SELECT sift.create_projection('tp', 'by_v', 'ORDER BY v DESC');
SELECT name, kind, state, rows FROM sift.projection_list() WHERE relation = 'tp'::regclass;
INSERT INTO tp SELECT i, date '2026-01-01' + i % 10, i FROM generate_series(1, 1000) i;
VACUUM tp;
SELECT name, kind, state, rows FROM sift.projection_list() WHERE relation = 'tp'::regclass;
SET sift.enable_agg = off;
SELECT p_uses('SELECT id, v FROM tp ORDER BY v DESC LIMIT 5', 'by_v');
SELECT p_same_order('SELECT id, v FROM tp ORDER BY v DESC LIMIT 5', 'SELECT id, v FROM tp ORDER BY v + 0 DESC LIMIT 5');
RESET sift.enable_agg;
SELECT sift.drop_projection('tp', 'by_v');
SET sift.test_create_format1 = on;
CREATE TABLE tp1 (id int, v int) USING sift_table;
SELECT sift.create_projection('tp1', 'by_v', 'ORDER BY v');
SET sift.test_create_format1 = off;
DROP TABLE p, s, w, tp, tp1;
DROP FUNCTION p_same(text), p_same_order(text, text), p_skipped(text), p_uses(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
