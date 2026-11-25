-- Spec 51: SiftHybridScan with runtime Params, the adaptive direction, array / case-folded predicates, residual quals, and a tenant EXISTS said as a key set.
-- Every result is compared with the same query that the hybrid scan refuses (a volatile qual is added), and the node and its direction are read from EXPLAIN.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE TABLE mm_docs (
	id int PRIMARY KEY, tenant int NOT NULL, region int NOT NULL, amount int NOT NULL, tags text[] NOT NULL,
	label text COLLATE "C" NOT NULL, body text NOT NULL, active boolean NOT NULL, seen timestamptz NOT NULL, note text, price numeric(12,2));
INSERT INTO mm_docs
SELECT i, i % 3000, i % 7, i % 100, ARRAY['t' || i % 13, 'u' || i % 5],
	CASE WHEN i % 997 = 0 THEN E'İstanbul' WHEN i % 1009 = 0 THEN E'Kelvin' ELSE 'Name' || i % 50 END,
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 20 = 0 THEN 'mid' END, CASE WHEN i % 6000 = 0 THEN 'rareterm' END, CASE WHEN i <= 6000 THEN 'clustered' END, 'tok' || i % 13, 'base'),
	i % 10 <> 0, timestamptz '2026-01-01' + i * interval '1 minute', CASE WHEN i % 11 = 0 THEN NULL ELSE 'n' || i END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE round(((i % 1000) / 8.0)::numeric, 2) END
FROM generate_series(1, 120000) i;
CREATE INDEX mm_text ON mm_docs USING sift (body);
CREATE INDEX mm_col ON mm_docs USING sift_col (id, tenant, region, amount, tags, label, active, seen, price) WITH (array_lengths = on);
CREATE TABLE mm_acl (tenant_id int NOT NULL, item_id int NOT NULL, deleted_at timestamptz);
INSERT INTO mm_acl SELECT 1, i, CASE WHEN i % 5 = 0 THEN now() END FROM generate_series(1, 30) i;
INSERT INTO mm_acl SELECT 2, i, NULL FROM generate_series(1, 120000, 3) i;
INSERT INTO mm_acl SELECT 4, i, NULL FROM generate_series(100, 400) i;
CREATE INDEX mm_acl_tenant ON mm_acl (tenant_id);
CREATE TABLE mm_args (n int, w text);
INSERT INTO mm_args VALUES (8, 'common'), (9, 'mid'), (20, 'tok3'), (99999, 'common'), (5, 'nosuchword');
ANALYZE mm_docs;
ANALYZE mm_acl;
VACUUM mm_docs;
RESET client_min_messages;
SET enable_nestloop = off;

CREATE FUNCTION hy_dir(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; d text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Hybrid Direction: ' THEN d := substring(l from 'Hybrid Direction: (.+)$'); END IF;
	END LOOP;
	RETURN d;
END $$;

CREATE FUNCTION hy_run(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; d text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Hybrid Direction: ' THEN d := substring(l from 'Hybrid Direction: (.+)$'); END IF;
	END LOOP;
	RETURN d;
END $$;

CREATE FUNCTION hy_prop(q text, label text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; v text := NULL;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ ('^\s*' || label || ': ') THEN v := substring(l from ': (\S+)$'); END IF;
	END LOOP;
	RETURN v;
END $$;

-- rows of a, and the rows that differ from b
CREATE FUNCTION hy_cmp(a text, b text, OUT rows bigint, OUT diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'CREATE TEMP TABLE hy_a AS ' || a;
	EXECUTE 'CREATE TEMP TABLE hy_b AS ' || b;
	SELECT count(*) INTO rows FROM hy_a;
	SELECT count(*) INTO diff FROM ((TABLE hy_a EXCEPT ALL TABLE hy_b) UNION ALL (TABLE hy_b EXCEPT ALL TABLE hy_a)) d;
	DROP TABLE hy_a, hy_b;
END $$;

-- q and the same query with a volatile qual, which the hybrid scan refuses
CREATE FUNCTION hy_same(q text) RETURNS TABLE (rows bigint, diff bigint) LANGUAGE sql AS
$$ SELECT * FROM hy_cmp(q, regexp_replace(q, 'WHERE ', 'WHERE random() < 2 AND ')) $$;

CREATE TABLE hy_q (name text PRIMARY KEY, q text);
INSERT INTO hy_q VALUES
	('const_point',	'SELECT id FROM mm_docs WHERE tenant = 8 AND body @@> ''common'''),
	('const_range',	'SELECT id FROM mm_docs WHERE tenant < 100 AND body @@> ''mid'''),
	('arr_overlap',	'SELECT id FROM mm_docs WHERE tags && ARRAY[''t3'', ''u1''] AND tenant < 300 AND body @@> ''common'''),
	('arr_contains','SELECT id FROM mm_docs WHERE tags @> ARRAY[''t3'', ''u3''] AND tenant < 300 AND body @@> ''common'''),
	('arr_empty',	'SELECT id FROM mm_docs WHERE tags && ARRAY[]::text[] AND tenant < 300 AND body @@> ''common'''),
	('arr_miss',	'SELECT id FROM mm_docs WHERE tags && ARRAY[''zz''] AND tenant < 300 AND body @@> ''common'''),
	('card',		'SELECT id FROM mm_docs WHERE cardinality(tags) > 1 AND tenant < 300 AND body @@> ''mid'''),
	('card_zero',	'SELECT id FROM mm_docs WHERE cardinality(tags) > 2 AND tenant < 300 AND body @@> ''mid'''),
	('lower_like',	'SELECT id FROM mm_docs WHERE lower(label) LIKE ''name1%'' AND tenant < 300 AND body @@> ''common'''),
	('lower_eq',	'SELECT id FROM mm_docs WHERE lower(label) = ''name12'' AND tenant < 900 AND body @@> ''common'''),
	('ilike',		'SELECT id FROM mm_docs WHERE label ILIKE ''NAME2%'' AND tenant < 300 AND body @@> ''common'''),
	('ilike_dotted','SELECT id FROM mm_docs WHERE label ILIKE ''istanbul'' AND body @@> ''common'''),
	('ilike_kelvin','SELECT id FROM mm_docs WHERE lower(label) LIKE ''kelvin%'' AND body @@> ''common'''),
	('residual',	'SELECT id FROM mm_docs WHERE tenant < 100 AND (amount + region) % 3 = 0 AND body @@> ''mid'''),
	('residual_arr','SELECT id FROM mm_docs WHERE tags && ARRAY[''t3''] AND (amount + region) % 3 = 0 AND tenant < 300 AND body @@> ''common'''),
	('in_list',		'SELECT id FROM mm_docs WHERE tenant IN (8, 9, 10, 11) AND body @@> ''common'''),
	('not_equal',	'SELECT id FROM mm_docs WHERE tenant < 100 AND region <> 3 AND body @@> ''mid'''),
	('ne_text',		'SELECT id FROM mm_docs WHERE tenant < 100 AND label <> ''Name3'' AND body @@> ''mid'''),
	('ne_int8',		'SELECT id FROM mm_docs WHERE tenant < 100 AND amount <> 7::int8 AND body @@> ''mid'''),
	('ne_bool',		'SELECT id FROM mm_docs WHERE tenant < 300 AND active <> true AND body @@> ''common'''),
	('ne_ts',		'SELECT id FROM mm_docs WHERE tenant < 100 AND seen <> timestamptz ''2026-01-02'' AND body @@> ''mid'''),
	('ne_null_rhs',	'SELECT id FROM mm_docs WHERE tenant < 100 AND region <> NULL AND body @@> ''mid'''),
	('ne_and_eq',	'SELECT id FROM mm_docs WHERE region <> 3 AND region <> 4 AND tenant < 100 AND body @@> ''mid'''),
	('num_gt',		'SELECT id FROM mm_docs WHERE price > 120 AND tenant < 3000 AND body @@> ''mid'''),
	('num_range',	'SELECT id FROM mm_docs WHERE price >= 12.5 AND price < 12.51 AND tenant < 3000 AND body @@> ''common'''),
	('num_eq',		'SELECT id FROM mm_docs WHERE price = 12.5 AND body @@> ''common'''),
	('num_eq_digits','SELECT id FROM mm_docs WHERE price = 12.505 AND body @@> ''common'''),
	('num_ne',		'SELECT id FROM mm_docs WHERE price <> 12.5 AND tenant < 3000 AND body @@> ''mid'''),
	('num_null',	'SELECT id FROM mm_docs WHERE price IS NULL AND tenant < 3000 AND body @@> ''mid'''),
	('num_notnull',	'SELECT id FROM mm_docs WHERE price IS NOT NULL AND tenant < 3000 AND body @@> ''mid'''),
	('num_output',	'SELECT id, price FROM mm_docs WHERE price > 100 AND tenant < 3000 AND tags && ARRAY[''t3''] AND body @@> ''common'''),
	('bool_ts',		'SELECT id FROM mm_docs WHERE active AND seen < timestamptz ''2026-01-05'' AND body @@> ''common'''),
	('null_test',	'SELECT id FROM mm_docs WHERE note IS NULL AND tenant < 100 AND body @@> ''mid'''),
	('empty_text',	'SELECT id FROM mm_docs WHERE tenant < 1000 AND body @@> ''nosuchword''');

\echo -- what the planner chooses, and the same rows as the standard plans
SELECT name, hy_dir(q) AS direction, (hy_same(q)).* FROM hy_q ORDER BY name;

\echo -- ranked: the top rows and their scores are those of the standard plans
CREATE TABLE hy_top (name text PRIMARY KEY, q text);
INSERT INTO hy_top VALUES
	('top_point',	'SELECT id, round(sift.score(ctid)::numeric, 5) AS s FROM mm_docs WHERE tenant = 8 AND body @@> ''common'' ORDER BY sift.score(ctid) DESC, id LIMIT 5'),
	('top_arr',		'SELECT id, round(sift.score(ctid)::numeric, 5) AS s FROM mm_docs WHERE tags && ARRAY[''t3''] AND tenant < 300 AND body @@> ''common tok3'' ORDER BY sift.score(ctid) DESC, id LIMIT 10'),
	('top_residual','SELECT id, round(sift.score(ctid)::numeric, 5) AS s FROM mm_docs WHERE tenant < 300 AND (amount + region) % 2 = 0 AND body @@> ''common'' ORDER BY sift.score(ctid) DESC, id LIMIT 10');
SELECT name, hy_dir(q) AS direction, (hy_same(q)).* FROM hy_top ORDER BY name;

\echo -- runtime Params: custom plans, then generic plans (the adaptive direction, chosen when the query is bound)
PREPARE p_point(int, text) AS SELECT id FROM mm_docs WHERE tenant = $1 AND body @@> $2;
PREPARE p_point_std(int, text) AS SELECT id FROM mm_docs WHERE random() < 2 AND tenant = $1 AND body @@> $2;
PREPARE p_range(int, text) AS SELECT id FROM mm_docs WHERE tenant < $1 AND body @@> $2;
PREPARE p_range_std(int, text) AS SELECT id FROM mm_docs WHERE random() < 2 AND tenant < $1 AND body @@> $2;
PREPARE p_arr(text[], text, text) AS SELECT id FROM mm_docs WHERE tags && $1 AND lower(label) LIKE $2 AND body @@> $3;
PREPARE p_arr_std(text[], text, text) AS SELECT id FROM mm_docs WHERE random() < 2 AND tags && $1 AND lower(label) LIKE $2 AND body @@> $3;
SELECT hy_dir('EXECUTE p_point(8, ''common'')') AS custom_point, hy_dir('EXECUTE p_range(100, ''mid'')') AS custom_range, hy_dir('EXECUTE p_arr(ARRAY[''t3''], ''name1%'', ''common'')') AS custom_arr;
SET plan_cache_mode = force_generic_plan;
SELECT hy_dir('EXECUTE p_point(8, ''common'')') AS generic_point, hy_dir('EXECUTE p_range(100, ''mid'')') AS generic_range, hy_dir('EXECUTE p_arr(ARRAY[''t3''], ''name1%'', ''common'')') AS generic_arr;
SELECT hy_run('EXECUTE p_point(8, ''common'')') AS bound_point, hy_run('EXECUTE p_point(8, ''mid'')') AS bound_point_mid, hy_run('EXECUTE p_range(100, ''mid'')') AS bound_rare, hy_run('EXECUTE p_range(2000, ''common'')') AS bound_wide;
SELECT a.args, (hy_cmp(format('EXECUTE p_point%s', a.args), format('EXECUTE p_point_std%s', a.args))).*
FROM (VALUES ('(8, ''common'')'), ('(9, ''mid'')'), ('(20, ''tok3'')'), ('(99999, ''common'')'), ('(5, ''nosuchword'')'), ('(1, ''base'')')) a(args);
SELECT a.args, (hy_cmp(format('EXECUTE p_range%s', a.args), format('EXECUTE p_range_std%s', a.args))).*
FROM (VALUES ('(100, ''mid'')'), ('(2000, ''common'')'), ('(0, ''common'')'), ('(3000, ''rareterm'')')) a(args);
SELECT a.args, (hy_cmp(format('EXECUTE p_arr%s', a.args), format('EXECUTE p_arr_std%s', a.args))).*
FROM (VALUES ('(ARRAY[''t3''], ''name1%'', ''common'')'), ('(ARRAY[''t3'', ''u2''], ''name2%'', ''mid'')'), ('(ARRAY[]::text[], ''name%'', ''common'')'), ('(ARRAY[''t1''], ''zzz%'', ''common'')')) a(args);
-- a NULL Param leaves no row
SELECT (hy_cmp('EXECUTE p_point(NULL, ''common'')', 'EXECUTE p_point_std(NULL, ''common'')')).*;
SELECT (hy_cmp('EXECUTE p_point(8, NULL)', 'EXECUTE p_point_std(8, NULL)')).*;
RESET plan_cache_mode;

\echo -- correlated rescans: a sub-select per row, and a LATERAL join
SELECT a.n, a.w, (SELECT count(*) FROM mm_docs WHERE tenant = a.n AND body @@> a.w) AS hits FROM mm_args a ORDER BY a.n;
SELECT a.n, count(c.id) AS hits FROM mm_args a, LATERAL (SELECT id FROM mm_docs WHERE tenant = a.n AND body @@> a.w) c GROUP BY a.n ORDER BY a.n;
SELECT count(*) AS correlated_diff FROM
	((SELECT a.n, (SELECT count(*) FROM mm_docs WHERE tenant = a.n AND body @@> a.w) FROM mm_args a)
	 EXCEPT ALL (SELECT a.n, (SELECT count(*) FROM mm_docs WHERE random() < 2 AND tenant = a.n AND body @@> a.w) FROM mm_args a)) d;
SELECT hy_dir('SELECT a.n, (SELECT count(*) FROM mm_docs WHERE tenant = a.n AND body @@> a.w) FROM mm_args a') AS rescan_direction;

\echo -- the tenant EXISTS, said as a key set: small, large (over the push cap), empty and deleted-only sets
CREATE TABLE hy_acl (name text PRIMARY KEY, q text);
INSERT INTO hy_acl VALUES
	('acl_small',	'SELECT c.id FROM mm_docs c WHERE c.body @@> ''common'' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 1 AND tl.deleted_at IS NULL AND tl.item_id = c.id)'),
	('acl_mid',		'SELECT c.id FROM mm_docs c WHERE c.body @@> ''common'' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 4 AND tl.item_id = c.id)'),
	('acl_large',	'SELECT c.id FROM mm_docs c WHERE c.body @@> ''mid'' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 2 AND tl.item_id = c.id)'),
	('acl_empty',	'SELECT c.id FROM mm_docs c WHERE c.body @@> ''common'' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 3 AND tl.item_id = c.id)'),
	('acl_arr',		'SELECT c.id FROM mm_docs c WHERE c.body @@> ''common'' AND c.tags && ARRAY[''t3'', ''t4''] AND lower(c.label) LIKE ''name1%'' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 4 AND tl.item_id = c.id)'),
	('acl_notexists','SELECT c.id FROM mm_docs c WHERE c.body @@> ''mid'' AND c.tenant < 200 AND NOT EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 1 AND tl.item_id = c.id)'),
	('acl_two',		'SELECT c.id FROM mm_docs c WHERE c.body @@> ''common'' AND EXISTS (SELECT 1 FROM mm_acl a WHERE a.tenant_id = 4 AND a.item_id = c.id) AND EXISTS (SELECT 1 FROM mm_acl b WHERE b.tenant_id = 2 AND b.item_id = c.id)');
SELECT name, hy_dir(q) AS direction, (hy_same(q)).* FROM hy_acl ORDER BY name;
SELECT hy_run(q) AS acl_small_bound FROM hy_acl WHERE name = 'acl_small';

\echo -- the tenant EXISTS keeps its semi join in every plan the hybrid scan is not part of
CREATE FUNCTION hy_shape(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; hybrid boolean := false; semi boolean := false; joined boolean := false; arr boolean := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		hybrid := hybrid OR l ~ 'SiftHybrid';
		semi := semi OR l ~ 'Semi Join';
		joined := joined OR l ~ '(Join|Nested Loop)';
		arr := arr OR l ~ '(InitPlan|SubPlan|ARRAY)';
	END LOOP;
	RETURN format('hybrid=%s joined=%s semi_join=%s array_subplan=%s', hybrid, joined, semi, arr);
END $$;
SELECT name, hy_shape(q) AS shape FROM hy_acl WHERE name IN ('acl_small', 'acl_mid', 'acl_large', 'acl_two') ORDER BY name;
SELECT name, regexp_replace(hy_shape(regexp_replace(q, 'WHERE ', 'WHERE random() < 2 AND ')), ' semi_join=[tf]', '') AS refused_shape FROM hy_acl ORDER BY name;
SELECT hy_shape('SELECT c.id FROM mm_docs c WHERE c.tenant < 100 AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 4 AND tl.item_id = c.id)') AS no_text_query;

\echo -- ranked and limited, with the EXISTS as a prepared Param
PREPARE p_acl(int, text) AS SELECT c.id, round(sift.score(c.ctid)::numeric, 5) AS s FROM mm_docs c WHERE c.body @@> $2 AND c.tags && ARRAY['t3', 't4'] AND lower(c.label) LIKE 'name1%'
	AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = $1 AND tl.deleted_at IS NULL AND tl.item_id = c.id) ORDER BY sift.score(c.ctid) DESC, c.id LIMIT 25;
PREPARE p_acl_std(int, text) AS SELECT c.id, round(sift.score(c.ctid)::numeric, 5) AS s FROM mm_docs c WHERE random() < 2 AND c.body @@> $2 AND c.tags && ARRAY['t3', 't4'] AND lower(c.label) LIKE 'name1%'
	AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = $1 AND tl.deleted_at IS NULL AND tl.item_id = c.id) ORDER BY sift.score(c.ctid) DESC, c.id LIMIT 25;
SELECT hy_dir('EXECUTE p_acl(4, ''common'')') AS custom_direction;
SELECT a.args, (hy_cmp(format('EXECUTE p_acl%s', a.args), format('EXECUTE p_acl_std%s', a.args))).*
FROM (VALUES ('(4, ''common'')'), ('(2, ''mid'')'), ('(1, ''common'')'), ('(3, ''common'')'), ('(4, ''nosuchword'')')) a(args);
SET plan_cache_mode = force_generic_plan;
SELECT hy_dir('EXECUTE p_acl(4, ''common'')') AS generic_direction;
SELECT a.args, (hy_cmp(format('EXECUTE p_acl%s', a.args), format('EXECUTE p_acl_std%s', a.args))).*
FROM (VALUES ('(4, ''common'')'), ('(2, ''mid'')'), ('(1, ''common'')'), ('(3, ''common'')'), ('(4, ''nosuchword'')')) a(args);
RESET plan_cache_mode;

\echo -- rows that change: deletes, updates (HOT and not), and the own writes of a transaction
BEGIN;
DELETE FROM mm_docs WHERE id % 4 = 0 AND tenant < 100;
UPDATE mm_docs SET note = 'edited' WHERE id % 6 = 0 AND tenant < 100;
UPDATE mm_docs SET tenant = 8 WHERE id % 50 = 1 AND tenant < 100;
INSERT INTO mm_docs VALUES (500001, 8, 1, 1, ARRAY['t3'], 'Name1', 'common fresh', true, now(), NULL);
SELECT name, (hy_same(q)).* FROM hy_q WHERE name IN ('const_point', 'const_range', 'arr_overlap', 'lower_like', 'residual', 'null_test') ORDER BY name;
SELECT count(*) AS own_row FROM mm_docs WHERE tenant = 8 AND tags && ARRAY['t3'] AND body @@> 'fresh';
ROLLBACK;
SELECT name, (hy_same(q)).* FROM hy_q WHERE name IN ('const_point', 'arr_overlap') ORDER BY name;

\echo -- a read-only transaction, a cursor, and a rescan of it
BEGIN READ ONLY;
SELECT hy_dir('SELECT id FROM mm_docs WHERE tenant = 8 AND body @@> ''common''') AS read_only_direction;
DECLARE hy_cur SCROLL CURSOR FOR SELECT id FROM mm_docs WHERE tenant = 8 AND tags && ARRAY['t3', 't4'] AND body @@> 'common' ORDER BY id;
FETCH 3 FROM hy_cur;
FETCH BACKWARD 1 FROM hy_cur;
FETCH ABSOLUTE 1 FROM hy_cur;
CLOSE hy_cur;
COMMIT;

\echo -- row level security: the policy is a security qual of the table, so the hybrid scan and the key set stay out of the way
CREATE ROLE hy_reader NOLOGIN;
GRANT SELECT ON mm_docs, mm_acl TO hy_reader;
ALTER TABLE mm_docs ENABLE ROW LEVEL SECURITY;
CREATE POLICY hy_even ON mm_docs FOR SELECT TO hy_reader USING (id % 2 = 0);
SET ROLE hy_reader;
SELECT hy_dir('SELECT id FROM mm_docs WHERE tenant = 8 AND body @@> ''common''') AS rls_direction;
SELECT hy_dir('SELECT c.id FROM mm_docs c WHERE c.body @@> ''common'' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 4 AND tl.item_id = c.id)') AS rls_acl_direction;
SELECT count(*) AS rls_rows FROM mm_docs WHERE tenant = 8 AND body @@> 'common';
SELECT count(*) AS rls_acl_rows FROM mm_docs c WHERE c.body @@> 'common' AND EXISTS (SELECT 1 FROM mm_acl tl WHERE tl.tenant_id = 4 AND tl.item_id = c.id);
RESET ROLE;
ALTER TABLE mm_docs DISABLE ROW LEVEL SECURITY;
DROP OWNED BY hy_reader;
DROP ROLE hy_reader;

\echo -- granules and parts the bridge skips, after a VACUUM
PREPARE hy_cl(int) AS SELECT id FROM mm_docs WHERE amount < $1 AND body @@> 'clustered';
PREPARE hy_cl_std(int) AS SELECT id FROM mm_docs WHERE random() < 2 AND amount < $1 AND body @@> 'clustered';
SET plan_cache_mode = force_generic_plan;
SELECT hy_dir('EXECUTE hy_cl(60)') AS direction,
	hy_prop('EXECUTE hy_cl(60)', 'Granules Skipped')::int > 0 AS skipped_some,
	hy_prop('EXECUTE hy_cl(60)', 'Granules Seen')::int > 0 AS seen_some,
	(hy_cmp('EXECUTE hy_cl(60)', 'EXECUTE hy_cl_std(60)')).*;
RESET plan_cache_mode;

\echo -- a partial index: the query has to imply the predicate
SET client_min_messages = warning;
CREATE TABLE mm_part (id int PRIMARY KEY, tenant int NOT NULL, live boolean NOT NULL, body text NOT NULL);
INSERT INTO mm_part SELECT i, i % 2000, i % 3 <> 0, concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, 'base') FROM generate_series(1, 30000) i;
CREATE INDEX mm_part_text ON mm_part USING sift (body);
CREATE INDEX mm_part_col ON mm_part USING sift_col (id, tenant) WHERE live;
ANALYZE mm_part;
RESET client_min_messages;
SELECT hy_dir('SELECT id FROM mm_part WHERE live AND tenant = 8 AND body @@> ''common''') AS implied,
	hy_dir('SELECT id FROM mm_part WHERE tenant = 8 AND body @@> ''common''') AS not_implied;
SELECT (hy_cmp('SELECT id FROM mm_part WHERE live AND tenant = 8 AND body @@> ''common''', 'SELECT id FROM mm_part WHERE random() < 2 AND live AND tenant = 8 AND body @@> ''common''')).*;
DROP FUNCTION hy_cmp(text, text), hy_dir(text), hy_prop(text, text), hy_run(text), hy_same(text), hy_shape(text);
DROP TABLE mm_docs, mm_part, mm_acl, mm_args, hy_acl, hy_q, hy_top;
DROP EXTENSION pg_sift;
