-- A sift_table with no sift_col index is read by the semi-join pushdown, the Top-N, the streaming aggregate, the window and DISTINCT ON of SiftAggV6, row for row as its heap twin is answered. Every check prints t. Run: psql -X -v ON_ERROR_STOP=1 -f this file, on a build with sift_table.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE ot (id int PRIMARY KEY, grp int, k int, tag text, at timestamptz, amount numeric(12,2), note text)
	USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE oh (LIKE ot INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE od (k int PRIMARY KEY, label text) USING sift_table;
CREATE TABLE odh (LIKE od INCLUDING ALL);

INSERT INTO ot
SELECT g, g % 17, g % 400, 'tag' || g % 5, timestamptz '2030-01-01' + g * interval '1 min',
	CASE WHEN g % 211 = 0 THEN NULL ELSE (g % 1000) / 8.0 END, 'note ' || g
FROM generate_series(1, 60000) g;
INSERT INTO od SELECT g, 'l' || g % 100 FROM generate_series(0, 399) g;
INSERT INTO oh SELECT * FROM ot;
INSERT INTO odh SELECT * FROM od;
SELECT sift.table_maintain('ot') IS NOT NULL AS maintained;
ANALYZE ot; ANALYZE oh; ANALYZE od; ANALYZE odh;

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

CREATE FUNCTION same_ordered(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n''), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n''), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

CREATE FUNCTION on_table(q text, twin boolean) RETURNS text LANGUAGE sql AS
$$ SELECT CASE WHEN twin THEN replace(replace(q, '@T', 'oh'), '@D', 'odh') ELSE replace(replace(q, '@T', 'ot'), '@D', 'od') END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_rows(on_table(q, false), on_table(q, true)) $$;

CREATE FUNCTION parity_ordered(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_ordered(on_table(q, false), on_table(q, true)) $$;

CREATE FUNCTION plan_has(q text, node text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || on_table(q, false) LOOP
		IF line LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the keys of a small dimension prune the fact table before PostgreSQL joins it
SELECT plan_has('SELECT f.id, f.grp FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5''', 'Custom Scan (SiftAggV6Sip)') AS sip_served_by_agg_v6;
SELECT plan_has('SELECT f.id, f.grp FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5''', 'Table: ot') AS sip_reads_the_table;
SELECT NOT plan_has('SELECT f.id, f.grp FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5''', 'Index:') AS sip_reads_no_index;
SELECT parity('SELECT f.id, f.grp, d.label FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5''') AS sip_one_label;
SELECT parity('SELECT f.id, f.tag, d.label FROM @T f JOIN @D d ON d.k = f.k WHERE d.label IN (''l5'', ''l77'') AND f.grp < 8') AS sip_with_fact_quals;
SELECT parity('SELECT f.id, f.amount FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5'' AND f.amount IS NOT NULL AND f.tag <> ''tag2''') AS sip_with_text_and_numeric_quals;
SELECT parity('SELECT d.label, count(*), sum(f.grp) FROM @T f JOIN @D d ON d.k = f.k WHERE d.label < ''l3'' GROUP BY d.label') AS sip_under_an_aggregate;
SELECT parity('SELECT f.id FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''nothing''') AS sip_no_dimension_row;

UPDATE ot SET k = (k + 1) % 400, grp = 3 WHERE id % 9 = 0;
UPDATE oh SET k = (k + 1) % 400, grp = 3 WHERE id % 9 = 0;
DELETE FROM ot WHERE id % 13 = 0;
DELETE FROM oh WHERE id % 13 = 0;
SELECT plan_has('SELECT f.id, f.grp FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5''', 'Custom Scan (SiftAggV6Sip)') AS sip_served_after_updates_and_deletes;
SELECT parity('SELECT f.id, f.grp, d.label FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = ''l5''') AS sip_after_updates_and_deletes;

DROP TABLE ot, oh, od, odh;
DROP FUNCTION parity(text), parity_ordered(text), same_rows(text, text), same_ordered(text, text), on_table(text, boolean), plan_has(text, text);
DROP EXTENSION pg_sift;
