-- Default operator classes of numeric, char(n), bpchar and text under a nondeterministic collation bind their typed classes and answer as the heap does (equality, ranges, IN, NaN and the infinities,
-- UNIQUE and ON CONFLICT, ordered reads), and sift.index_choices, sift.col_sections and sift.index_size_breakdown answer for Format 2.0 (empty, slab, parts, object storage) without an error.
-- Format 1 (the testing build only: its indexes need sift.test_create_format1) is checked in a block that prints nothing, so the output is the same on both builds.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
CREATE COLLATION oc_nd (provider = icu, locale = 'und-u-ks-level2', deterministic = false);

SELECT opcname, opcdefault FROM pg_opclass WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'sift_col') AND opcname IN ('col_numeric_ops', 'col_bpchar_ops', 'col_text_ops', 'col_ops') ORDER BY 1;

-- one function builds the table and its default indexes in either format, and says which shapes the index and the heap disagree on
CREATE FUNCTION oc_check(f1 bool) RETURNS TABLE (flavor text, shapes int, mismatches int, ordered_mismatches int, classes text) LANGUAGE plpgsql AS $$
DECLARE
	tbl text := CASE WHEN f1 THEN 'oc_f1' ELSE 'oc_f2' END;
	w text; a text; b text; bad int := 0; n int := 0; ord int := 0; cls text;
	shapes text[] := ARRAY['n = 12.5', 'n = 12.50', 'n > 1000', 'n < 5', 'n = ''NaN''', 'n = ''Infinity''', 'n < ''-Infinity''', 'n >= ''-Infinity''', 'n IS NULL', 'n IN (1.25, 2.5, ''NaN'')', 'n <> 12.5',
		'n = 1e300', 'n2 = 0.33', 'n2 BETWEEN 1 AND 2', 'n3 = 1000000.123456', 'n3 > 1000000000', 'n3 = 1200 * 1000000.123456', 'n3 IN (1000000.123456, 2000000.246912)',
		'c8 = ''ab7''', 'c8 = ''ab7     ''', 'c8 > ''ab5''', 'c8 IN (''ab1'', ''ab2'')', 'bp = ''x3''', 'bp = ''x3  ''', 'bp > ''x4''', 'bp IN (''x1'', ''x2'')',
		't = ''t5''', 't = ''T5''', 't = ''t5 ''', 't > ''t2''', 't IN (''t1'', ''T2'')', 't IS NULL'];
BEGIN
	IF f1 THEN PERFORM set_config('sift.test_create_format1', 'on', true); END IF;
	EXECUTE format('CREATE TABLE %I (id int, n numeric, n2 numeric(10,2), n3 numeric(38,6), c8 char(8), bp bpchar, t text COLLATE oc_nd)', tbl);
	EXECUTE format($q$INSERT INTO %I SELECT g, CASE g %% 7 WHEN 0 THEN 'NaN'::numeric WHEN 1 THEN NULL ELSE g * 1.25 END, g / 3.0, g * 1000000.123456, 'ab' || g %% 100, 'x' || g %% 50 || '  ',
		CASE WHEN g %% 40 = 0 THEN NULL ELSE 'T' || g %% 30 END FROM generate_series(1, 1200) g$q$, tbl);
	EXECUTE format($q$INSERT INTO %I VALUES (1201, 'Infinity', 1, 1, 'a', 'a', 'a'), (1202, '-Infinity', 1, 1, 'a', 'a', 'a'), (1203, 1e300, 1, 1, 'a', 'a', 'a')$q$, tbl);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (n, n2, n3, c8, bp, t)', tbl || '_i', tbl);
	EXECUTE format('ANALYZE %I', tbl);
	SELECT string_agg(o.opcname, ' ' ORDER BY a.ord) INTO cls FROM pg_index i, unnest(i.indclass) WITH ORDINALITY a(oid, ord) JOIN pg_opclass o ON o.oid = a.oid WHERE i.indexrelid = (tbl || '_i')::regclass;
	FOREACH w IN ARRAY shapes LOOP
		n := n + 1;
		EXECUTE format($q$SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '') FROM %I WHERE %s$q$, tbl, w) INTO a;
		PERFORM set_config('enable_seqscan', 'off', true);
		EXECUTE format($q$SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '') FROM %I WHERE %s$q$, tbl, w) INTO b;
		PERFORM set_config('enable_seqscan', 'on', true);
		bad := bad + (a <> b)::int;
	END LOOP;
	FOREACH w IN ARRAY ARRAY['n', 'n2', 'n3', 'c8', 'bp', 't'] LOOP
		EXECUTE format($q$SELECT string_agg(id::text, ',') FROM (SELECT id FROM %I ORDER BY %s, id LIMIT 40) s$q$, tbl, w) INTO a;
		PERFORM set_config('enable_seqscan', 'off', true);
		EXECUTE format($q$SELECT string_agg(id::text, ',') FROM (SELECT id FROM %I ORDER BY %s, id LIMIT 40) s$q$, tbl, w) INTO b;
		PERFORM set_config('enable_seqscan', 'on', true);
		ord := ord + (a <> b)::int;
	END LOOP;
	RETURN QUERY SELECT CASE WHEN f1 THEN 'f1' ELSE 'f2' END, n, bad, ord, cls;
END $$;
SET sift.async_maintenance = off;
SELECT * FROM oc_check(false);
DO $$
DECLARE r record;
BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		SELECT * INTO r FROM oc_check(true);
		IF r.mismatches <> 0 OR r.ordered_mismatches <> 0 OR r.classes <> 'col_numeric_ops col_numeric_ops col_numeric_ops col_bpchar_ops col_bpchar_ops col_text_ops' THEN
			RAISE EXCEPTION 'format 1: % mismatches, % ordered, classes %', r.mismatches, r.ordered_mismatches, r.classes;
		END IF;
		IF (SELECT major FROM sift.index_format('oc_f1_i')) <> 1 THEN RAISE EXCEPTION 'oc_f1_i is not format 1'; END IF;
	END IF;
END $$;

-- UNIQUE and ON CONFLICT on every numeric shape; a char(n), a bpchar and a text of a nondeterministic collation are refused as keys of a unique index, by name
CREATE TABLE ocu (k numeric, k2 numeric(10,2), k3 numeric(38,6), v int);
CREATE UNIQUE INDEX ocu_k ON ocu USING sift_col (k);
CREATE UNIQUE INDEX ocu_k2 ON ocu USING sift_col (k2);
CREATE UNIQUE INDEX ocu_k3 ON ocu USING sift_col (k3);
INSERT INTO ocu SELECT g * 1.5, g * 0.25, g * 1000000.000001, 1 FROM generate_series(1, 500) g;
INSERT INTO ocu VALUES ('NaN', 1.01, 1.000001, 1), ('Infinity', 1.02, 1.000002, 1), ('-Infinity', 1.03, 1.000003, 1);
INSERT INTO ocu VALUES (3.0, 9.99, 9.999999, 2) ON CONFLICT (k) DO NOTHING;
INSERT INTO ocu VALUES (3.00, 9.99, 9.999999, 3) ON CONFLICT (k) DO UPDATE SET v = ocu.v + 10 RETURNING k, v;
INSERT INTO ocu VALUES (7, 0.50, 9.999999, 4) ON CONFLICT (k2) DO UPDATE SET v = ocu.v + 20 RETURNING k2, v;
INSERT INTO ocu VALUES (7, 9.98, 1000000.000001, 5) ON CONFLICT (k3) DO UPDATE SET v = ocu.v + 30 RETURNING k3, v;
INSERT INTO ocu VALUES ('NaN', 9.97, 9.999998, 6) ON CONFLICT (k) DO UPDATE SET v = ocu.v + 40 RETURNING k, v;
INSERT INTO ocu VALUES ('Infinity', 9.96, 9.999997, 7) ON CONFLICT (k) DO UPDATE SET v = ocu.v + 50 RETURNING k, v;
INSERT INTO ocu VALUES (4.5, 9.95, 9.999996, 8);
SELECT count(*) AS rows, count(DISTINCT k) AS k, count(DISTINCT k2) AS k2, count(DISTINCT k3) AS k3 FROM ocu;
CREATE TABLE ocu2 (c char(8), b bpchar, t text COLLATE oc_nd);
CREATE UNIQUE INDEX ocu2_c ON ocu2 USING sift_col (c);
CREATE UNIQUE INDEX ocu2_b ON ocu2 USING sift_col (b);
CREATE UNIQUE INDEX ocu2_t ON ocu2 USING sift_col (t);
CREATE INDEX ocu2_all ON ocu2 USING sift_col (c, b, t);

-- sift.index_choices, sift.col_sections and sift.index_size_breakdown of Format 2.0: an empty index, rows in the slab, parts, key order and parts in object storage
CREATE FUNCTION oc_report(rel regclass) RETURNS TABLE (what text, answer text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT 'choices', string_agg(DISTINCT kind, ',') || ' ' || count(*) FILTER (WHERE slug IS NOT NULL) || ' slug' FROM sift.index_choices(rel);
	RETURN QUERY SELECT 'choice rows', string_agg(c, ',' ORDER BY c) FROM (SELECT DISTINCT CASE WHEN choice LIKE 'encoding(%' THEN 'encoding' ELSE choice END AS c FROM sift.index_choices(rel)) q;
	RETURN QUERY SELECT 'sections sum', (sum(bytes) = pg_relation_size(rel))::text FROM sift.col_sections(rel);
	RETURN QUERY SELECT 'breakdown sum', (sum(bytes) = pg_relation_size(rel))::text FROM sift.index_size_breakdown(rel);
END $$;
CREATE TABLE ocf (id int8, ts timestamp NOT NULL, v int, n numeric, t text);
CREATE INDEX ocf_col ON ocf USING sift_col (id, ts, v, n, t);
SELECT * FROM oc_report('ocf_col');
INSERT INTO ocf SELECT g, timestamp '2020-01-01' + g * interval '1 hour', g % 100, g * 1.5, md5(g::text) FROM generate_series(1, 20) g;
SELECT value LIKE '1 partitions hold rows in an L0 slab%' AS in_slab FROM sift.index_choices('ocf_col') WHERE choice = 'l0_slab';
SELECT * FROM oc_report('ocf_col') WHERE what = 'sections sum';
INSERT INTO ocf SELECT g, timestamp '2020-01-01' + g * interval '1 hour', g % 100, g * 1.5, md5(g::text) FROM generate_series(21, 3000) g;
SELECT sift.maintain('ocf_col');
SELECT * FROM oc_report('ocf_col');
SELECT choice, value ~ '^[a-z0-9_]+ x[0-9]+' AS names_encodings, cost ~ '^[0-9]+ bytes$' AS has_cost FROM sift.index_choices('ocf_col') WHERE choice LIKE 'encoding(%' ORDER BY choice;
CREATE INDEX ocf_keyed ON ocf USING sift_col (id, ts, v, n, t) WITH (key_order = 'ts');
SELECT choice, value FROM sift.index_choices('ocf_keyed') WHERE choice IN ('key_order', 'layout') ORDER BY choice;
SELECT value LIKE 'none%' AS unkeyed, off IS NULL AS no_command FROM sift.index_choices('ocf_col') WHERE choice = 'key_order';

INSERT INTO ocf SELECT 3000 + g, localtimestamp - g * interval '1 minute', g % 100, g * 1.5, md5((3000 + g)::text) FROM generate_series(1, 1500) g;
VACUUM ocf;
SELECT current_setting('data_directory') || '/sift_oc_tier' AS tierdir \gset
\set mk 'rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
SELECT sift.set_tier_policy('ocf_col', 'file://' || :'tierdir', '30 days') AS changed;
VACUUM ocf;
SELECT count(*) > 0 AS tiered FROM sift.tier_status;
SELECT choice, value ~ '^[0-9]+ parts in object storage' AS counted FROM sift.index_choices('ocf_col') WHERE choice = 'object_storage';
SELECT * FROM oc_report('ocf_col') WHERE what LIKE '% sum';
SELECT count(*) AS problems FROM sift.check('ocf_col');

-- a sift_table of Format 2.0: its choices and sections too
CREATE TABLE oct (id int, n numeric, t text) USING sift_table;
INSERT INTO oct SELECT g, g * 1.25, 'item number ' || (g % 5000) FROM generate_series(1, 12000) g;
VACUUM oct;
SELECT choice FROM sift.index_choices('oct') ORDER BY choice;
SELECT * FROM oc_report('oct') WHERE what LIKE '% sum';
SELECT count(*) > 0 AS sections FROM sift.col_sections('oct') WHERE bytes > 0;

-- errors that stay errors: a relation that is no sift relation
SELECT sift.index_choices('ocu'::regclass);
CREATE INDEX oc_btree ON ocu (v);
SELECT sift.index_choices('oc_btree'::regclass);
SELECT sift.col_sections('oc_btree'::regclass);

-- Format 1, the testing build only: no error, the same checks, and nothing quarantined
DO $$
BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'on', true);
		CREATE TABLE ocg (id int, n numeric, t text);
		INSERT INTO ocg SELECT g, g * 1.5, md5(g::text) FROM generate_series(1, 500) g;
		CREATE INDEX ocg_i ON ocg USING sift_col (id, n, t);
		PERFORM * FROM sift.index_choices('ocg_i');
		IF (SELECT sum(bytes) FROM sift.col_sections('ocg_i')) <> pg_relation_size('ocg_i') THEN RAISE EXCEPTION 'format 1 sections do not add up'; END IF;
		BEGIN
			PERFORM * FROM sift.index_size_breakdown('ocg_i');
			RAISE EXCEPTION 'index_size_breakdown answered for a format 1 index';
		EXCEPTION WHEN wrong_object_type THEN NULL;
		END;
		IF (SELECT count(*) FROM sift.check('ocg_i')) <> 0 THEN RAISE EXCEPTION 'format 1 check'; END IF;
	END IF;
END $$;
SELECT count(*) AS quarantined FROM sift.status() WHERE quarantined;
DROP TABLE ocf, oct, ocu, ocu2, oc_f2;
DROP TABLE IF EXISTS oc_f1, ocg;
DROP FUNCTION oc_check(bool), oc_report(regclass);
DROP COLLATION oc_nd;
\set rm 'rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift;
