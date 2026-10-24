-- key_cut: the units of a sift_col index are cut by the range of a declared key (column format minor 17, docs/COL.md 2.16), not by superblock, and list the heap
-- addresses of their rows. A lookup or a range of the key reads the units of its range whatever the order of the heap; the rows are those of the normal plan after
-- rows in a write buffer, a promotion, deletes and updates, VACUUM, a merge, a refresh and a REINDEX; a lowered sift.col_format_minor writes superblock units again.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

-- 600 customers in a random heap order, 100,000 rows
CREATE TABLE kc (id int, cust int, d date, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO kc SELECT i, (i * 7919) % 600, date '2026-01-01' + (i * 31) % 300, i % 100, repeat('x', 100) FROM generate_series(1, 100000) i;
CREATE INDEX kc_idx ON kc USING sift_col (id, cust, d, amount) WITH (key_cut = 'cust, d');
CREATE TABLE kp (LIKE kc);
INSERT INTO kp SELECT * FROM kc;
CREATE INDEX kp_idx ON kp USING sift_col (id, cust, d, amount);
VACUUM (FREEZE, ANALYZE) kc;
VACUUM (FREEZE, ANALYZE) kp;
SELECT sift.col_verify('kc_idx', true);

-- the segment is column format minor 17 with units of about 1,024 rows; the index without the option is not
SELECT kind, format, units > 90 AS many_units FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
SELECT kind, format FROM sift.col_segments('kp_idx') WHERE kind = 'immutable';

CREATE FUNCTION kchk(step text) RETURNS TABLE (after_step text, shape text, result text) LANGUAGE plpgsql AS $$
DECLARE r record; a text; b text; u text; l text; q text;
BEGIN
	FOR r IN SELECT * FROM (VALUES
		('one customer', 'SELECT count(*), sum(amount), min(id) FROM kc WHERE cust = 42'),
		('customer and dates', 'SELECT count(*), sum(amount), min(id) FROM kc WHERE cust = 42 AND d >= date ''2026-03-01'' AND d < date ''2026-05-01'''),
		('customer range', 'SELECT count(*), sum(amount), min(id) FROM kc WHERE cust BETWEEN 100 AND 103'),
		('customer list', 'SELECT count(*), sum(amount), min(id) FROM kc WHERE cust IN (7, 300, 599)'),
		('other column', 'SELECT count(*), sum(amount) FROM kc WHERE amount = 5'),
		('top-N by key', 'SELECT id, cust, d FROM kc ORDER BY cust, d, id LIMIT 5')) v(s, qq)
	LOOP
		q := r.qq; u := '';
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l LIKE '%Units: %' OR l LIKE '%Units Pruned: %' THEN u := u || ' ' || trim(l); END IF;
		END LOOP;
		EXECUTE format('SELECT string_agg(x::text, E'';'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL sift.enable_topn = off;
		EXECUTE format('SELECT string_agg(x::text, E'';'') FROM (%s) x', q) INTO b;
		RESET sift.enable_agg; RESET sift.enable_topn;
		after_step := step; shape := r.s;
		result := CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE 'DIFFERENT' END || u;
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM kchk('built');
-- the same customer in the index with superblock units: the random heap puts its rows in every unit
CREATE FUNCTION kunits(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; u text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Units: %' OR l LIKE '%Units Pruned: %' THEN u := u || ' ' || trim(l); END IF;
	END LOOP;
	RETURN trim(u);
END $$;
SELECT kunits('SELECT count(*), sum(amount) FROM kp WHERE cust = 42') AS superblock_units;
INSERT INTO kc SELECT 200000 + i, 42, date '2026-04-01', 7, 'y' FROM generate_series(1, 30) i;
SELECT * FROM kchk('write buffer');
SELECT sift.col_promote('kc_idx') > 0 AS promoted;
SELECT * FROM kchk('promoted');
DELETE FROM kc WHERE id % 7 = 0;
UPDATE kc SET amount = amount + 1, cust = 42 WHERE id % 1009 = 0;
SELECT * FROM kchk('deleted and updated');
VACUUM kc;
SELECT * FROM kchk('vacuumed');
SELECT sift.col_merge('kc_idx', true) >= 0 AS merged;
SELECT * FROM kchk('merged');
SELECT count(*) >= 0 AS refreshed FROM sift.col_refresh('kc_idx');
SELECT * FROM kchk('refreshed');
REINDEX INDEX kc_idx;
SELECT * FROM kchk('reindexed');
SELECT sift.col_verify('kc_idx', true);
SELECT DISTINCT format FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';

-- the setting that rolls a 3.3 index back to a release that cannot read minor 17 writes superblock units again
SET sift.col_format_minor = 14;
REINDEX INDEX kc_idx;
SELECT DISTINCT format FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
SELECT * FROM kchk('lowered setting');
RESET sift.col_format_minor;
REINDEX INDEX kc_idx;
SELECT DISTINCT format FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';

-- a build that passes its memory writes a sorted segment for each run of rows, and a merge streams the runs into one segment: the rows stay those of the normal plan
DROP INDEX kc_idx;
SET sift.test_col_segment_kb = 64;
CREATE INDEX kc_idx ON kc USING sift_col (id, cust, d, amount) WITH (key_cut = 'cust, d');
RESET sift.test_col_segment_kb;
SELECT count(*) > 1 AS runs, min(format) AS format FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
SELECT * FROM kchk('runs');
SET maintenance_work_mem = '1MB';
SELECT sift.col_merge('kc_idx', true) >= 0 AS merged;
RESET maintenance_work_mem;
SELECT count(*) AS segments, min(format) AS format FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
SELECT * FROM kchk('runs merged');
SELECT sift.col_verify('kc_idx', true);

-- an index that gets the option later has segments cut by superblock: a merge loads and sorts them, and the rows written since are in key-cut segments
CREATE TABLE kl (LIKE kc);
INSERT INTO kl SELECT * FROM kc WHERE id <= 60000;
CREATE INDEX kl_idx ON kl USING sift_col (id, cust, d, amount);
ALTER INDEX kl_idx SET (key_cut = 'cust, d');
INSERT INTO kl SELECT * FROM kc WHERE id > 60000;
SELECT sift.col_promote('kl_idx');
SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) AS formats FROM sift.col_segments('kl_idx') WHERE kind = 'immutable';
SELECT sift.col_merge('kl_idx', true) >= 0 AS merged;
SELECT string_agg(DISTINCT format::text, ',') AS formats, count(*) AS segments FROM sift.col_segments('kl_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('kl_idx', true);
SELECT (SELECT count(*) || ' ' || sum(amount) FROM kl WHERE cust = 42) = (SELECT count(*) || ' ' || sum(amount) FROM kc WHERE cust = 42) AS same_rows;
SELECT sift.col_verify('kc_idx', true);
DROP TABLE kl;

DROP FUNCTION kchk(text), kunits(text);
DROP TABLE kc, kp;
DROP EXTENSION pg_sift;
