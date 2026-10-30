-- ORDER BY <column> LIMIT n over a sift_col index when updated rows have moved: the new version of a row is stored wherever the heap has room
-- (the pages that DELETE and VACUUM freed in the middle of the table), with a value that has nothing to do with its neighbours. The zones of a few
-- superblocks must not decide that the rest hold nothing better. Every query runs twice, the plain plan as the reference (sift.enable_agg off)
-- and the pushed one, and the rows are compared in order.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;

-- pad makes a page hold about 5 rows: 24000 rows are about 19 superblocks, more than the zones the scan probes before it starts
CREATE TABLE hc (id serial PRIMARY KEY, seq int, g int, pad text DEFAULT repeat('x', 1500));
ALTER TABLE hc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO hc (seq, g) SELECT i * 3, i % 5 FROM generate_series(1, 24000) i;
CREATE INDEX hc_col ON hc USING sift_col (id, seq, g);
VACUUM (ANALYZE) hc;

CREATE FUNCTION hc_mode(pushed bool) RETURNS void LANGUAGE sql AS $$ SELECT sift_helpers.agg_mode(pushed) $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan
CREATE FUNCTION hc_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text;
BEGIN
	DROP TABLE IF EXISTS hc_a, hc_b;
	PERFORM hc_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE hc_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM hc_mode(false);
	EXECUTE 'CREATE TEMP TABLE hc_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM hc_a EXCEPT ALL SELECT pos, r FROM hc_b) UNION ALL (SELECT pos, r FROM hc_b EXCEPT ALL SELECT pos, r FROM hc_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || ' ' || (SELECT count(*) FROM hc_b);
END $$;

CREATE TABLE hc_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO hc_q VALUES
	('asc',       $q$SELECT id, seq FROM hc ORDER BY seq, id LIMIT 5$q$),
	('desc',      $q$SELECT id, seq FROM hc ORDER BY seq DESC, id LIMIT 5$q$),
	('asc_g',     $q$SELECT id, seq FROM hc WHERE g = 2 ORDER BY seq, id LIMIT 5$q$),
	('desc_g',    $q$SELECT id, seq FROM hc WHERE g IN (0, 1) ORDER BY seq DESC, id LIMIT 5$q$),
	('nulls',     $q$SELECT id, seq FROM hc ORDER BY seq NULLS FIRST, id LIMIT 5$q$),
	('two_keys',  $q$SELECT id, g, seq FROM hc ORDER BY g DESC, seq, id LIMIT 5$q$);

CREATE FUNCTION hc_stage(name text) RETURNS TABLE (stage text, results text) LANGUAGE sql AS $$
	SELECT name, string_agg(tag || '=' || hc_check(q), E'\n' ORDER BY tag COLLATE "C") FROM hc_q $$;

SELECT * FROM hc_stage('build');

-- the pages of ids 8000 to 8200 are freed: the heap puts the new versions of the updated rows there, in the middle of the table
DELETE FROM hc WHERE id BETWEEN 8000 AND 8200;
VACUUM hc;
UPDATE hc SET seq = -5 WHERE id = 5;
UPDATE hc SET seq = 999999 WHERE id = 6;
UPDATE hc SET seq = NULL WHERE id = 7;
UPDATE hc SET seq = -9, g = 9 WHERE id = 11;
SELECT id, ctid::text ~ '^\(4[0-9]{3},' AS moved FROM hc WHERE id IN (5, 6, 7, 11) ORDER BY id;
SELECT * FROM hc_stage('moved');

-- and again after the index sees the new versions as stored units
VACUUM hc;
SELECT sift.col_promote('hc_col') >= 0 AS promoted;
SELECT * FROM hc_stage('promoted');

DROP TABLE hc_q, hc;
DROP FUNCTION hc_mode(bool), hc_check(text), hc_stage(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
