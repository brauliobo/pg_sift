-- A sift_table with no sift_col index is read by the row Top-N, the star join and the unnest of SiftAggV6, row for row as its heap twin is answered. Every check prints t. Run: psql -X -v ON_ERROR_STOP=1 -f this file, on a build with sift_table.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE rt (id int PRIMARY KEY, grp int, k int, tag text, amount numeric(12,2), tags text[], ids int[], note text)
	USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE rh (LIKE rt INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE rd (k int PRIMARY KEY, label text) USING sift_table;
CREATE TABLE rdh (LIKE rd INCLUDING ALL);

INSERT INTO rt
SELECT g, g % 17, g % 40, 'tag' || g % 5, CASE WHEN g % 211 = 0 THEN NULL ELSE (g % 1000) / 8.0 END,
	CASE g % 7 WHEN 0 THEN NULL WHEN 1 THEN '{}' ELSE ARRAY['a' || g % 3, 'b' || g % 5] END,
	CASE g % 6 WHEN 0 THEN NULL WHEN 1 THEN '{}' ELSE ARRAY[g % 4, g % 9, NULL] END,
	'note ' || g
FROM generate_series(1, 30000) g;
INSERT INTO rd SELECT g, 'l' || g % 8 FROM generate_series(0, 39) g;
INSERT INTO rh SELECT * FROM rt;
INSERT INTO rdh SELECT * FROM rd;
SELECT sift.table_maintain('rt') IS NOT NULL AS maintained;
ANALYZE rt; ANALYZE rh; ANALYZE rd; ANALYZE rdh;

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_rows(replace(replace(q, '@T', 'rt'), '@D', 'rd'), replace(replace(q, '@T', 'rh'), '@D', 'rdh')) $$;

CREATE FUNCTION plan_has(q text, node text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(replace(q, '@T', 'rt'), '@D', 'rd') LOOP
		IF line LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- ORDER BY .. LIMIT of the rows of one table
SELECT plan_has('SELECT id, tag FROM @T WHERE k < 10 ORDER BY tag, id LIMIT 5', 'Custom Scan (SiftAggV6)') AS rows_served_by_agg_v6;
SELECT plan_has('SELECT id, tag FROM @T WHERE k < 10 ORDER BY tag, id LIMIT 5', 'Table: rt') AS rows_read_from_the_table;
SELECT parity('SELECT id, tag, amount FROM @T WHERE k < 10 ORDER BY tag, id LIMIT 5') AS rows_asc;
SELECT parity('SELECT id, note FROM @T WHERE grp = 3 ORDER BY tag DESC, id LIMIT 20') AS rows_desc;
SELECT parity('SELECT id, tag FROM @T WHERE grp IN (1, 4) AND amount IS NOT NULL ORDER BY tag, id DESC LIMIT 50') AS rows_two_keys_and_quals;
SELECT parity('SELECT id, note FROM @T WHERE grp > 100 ORDER BY tag, id LIMIT 5') AS rows_no_match;

UPDATE rt SET tag = 'tag9', grp = 3 WHERE id % 9 = 0;
UPDATE rh SET tag = 'tag9', grp = 3 WHERE id % 9 = 0;
DELETE FROM rt WHERE id % 13 = 0;
DELETE FROM rh WHERE id % 13 = 0;
SELECT parity('SELECT id, tag FROM @T WHERE grp = 3 ORDER BY tag DESC, id LIMIT 30') AS rows_after_updates_and_deletes;

DROP TABLE rt, rh, rd, rdh;
DROP FUNCTION parity(text), same_rows(text, text), plan_has(text, text);
DROP EXTENSION pg_sift;
