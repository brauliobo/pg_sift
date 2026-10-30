-- A search on a table whose units are cut by a key (column format minor 17): the matches are heap addresses, and the scan keeps of each unit the rows at those addresses
-- from the packed directory of the unit's pages (the rows of a heap page lie in many units). The rows are those of the normal plan for a word in a few rows, in a fifth of
-- them and in all but a few (the filter keeps whole pages), after rows in a write buffer, a promotion, deletes and updates, VACUUM and a merge.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- 600 customers in a random heap order, 60,000 rows
CREATE TABLE ks (id int, cust int, d date, amount int, memo text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ks SELECT i, (i * 7919) % 600, date '2026-01-01' + (i * 31) % 300, i % 100,
       CASE WHEN i % 1000 = 0 THEN 'rareword ' ELSE '' END || (ARRAY['cable', 'wire', 'plug', 'switch', 'relay'])[1 + i % 5] || CASE WHEN i % 9 <> 0 THEN ' common' ELSE '' END, repeat('x', 100)
FROM generate_series(1, 60000) i;
CREATE INDEX ks_col ON ks USING sift_col (id, cust, d, amount) WITH (key_cut = 'cust, d');
CREATE INDEX ks_memo ON ks USING sift (memo);
VACUUM (FREEZE, ANALYZE) ks;
SELECT sift.col_verify('ks_col', true);
SELECT kind, format FROM sift.col_segments('ks_col') WHERE kind = 'immutable';

CREATE FUNCTION kscheck(step text) RETURNS TABLE (after_step text, shape text, result text) LANGUAGE plpgsql AS $$
DECLARE r record; a text; b text; l text; m text;
BEGIN
	FOR r IN SELECT * FROM (VALUES
		('a few rows', 'SELECT cust % 5, count(*), sum(amount) FROM ks WHERE memo @@> ''rareword'' GROUP BY 1 ORDER BY 1'),
		('a fifth', 'SELECT cust % 5, count(*), sum(amount) FROM ks WHERE memo @@> ''cable'' GROUP BY 1 ORDER BY 1'),
		('most rows', 'SELECT cust % 5, count(*), sum(amount) FROM ks WHERE memo @@> ''common'' GROUP BY 1 ORDER BY 1'),
		('two words', 'SELECT count(*), sum(amount), min(id) FROM ks WHERE memo @@> ''cable common'''),
		('word and a test', 'SELECT d, count(*) FROM ks WHERE memo @@> ''wire'' AND amount >= 50 GROUP BY d ORDER BY 1 LIMIT 5'),
		('word and a range of the key', 'SELECT count(*), sum(amount) FROM ks WHERE memo @@> ''plug'' AND cust BETWEEN 100 AND 130'),
		('word, no match', 'SELECT count(*) FROM ks WHERE memo @@> ''nothing''')) v(s, qq)
	LOOP
		m := '';
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || r.qq LOOP
			IF l LIKE '%search matches%' THEN m := 'search'; END IF;
		END LOOP;
		EXECUTE format('SELECT string_agg(x::text, E'';'') FROM (%s) x', r.qq) INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE format('SELECT string_agg(x::text, E'';'') FROM (%s) x', r.qq) INTO b;
		RESET sift.enable_agg;
		after_step := step; shape := r.s;
		result := CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE 'DIFFERENT' END || ' ' || m;
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM kscheck('built');
INSERT INTO ks SELECT 100000 + i, 42, date '2026-04-01', 7, 'cable common extra', 'y' FROM generate_series(1, 30) i;
SELECT * FROM kscheck('write buffer');
SELECT sift.col_promote('ks_col') > 0 AS promoted;
SELECT * FROM kscheck('promoted');
DELETE FROM ks WHERE id % 7 = 0;
UPDATE ks SET amount = amount + 1, cust = 42, memo = 'cable wire' WHERE id % 1009 = 0;
SELECT * FROM kscheck('deleted and updated');
VACUUM ks;
SELECT * FROM kscheck('vacuumed');
SELECT sift.col_merge('ks_col', true) >= 0 AS merged;
SELECT * FROM kscheck('merged');
SELECT sift.col_verify('ks_col', true);

DROP FUNCTION kscheck(text);
DROP TABLE ks;
DROP EXTENSION pg_sift;
