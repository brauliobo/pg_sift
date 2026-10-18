-- Plan choice of the filter-mask pushdown for a filter on a value that the table stores in one stretch of the heap
-- (rows loaded by group). The column's statistics cannot place it: its correlation is low whenever the groups are not in
-- sort order. A scan that checks the filter after the ranker meets the matches in heap order and verifies every one of
-- them before the first rows that pass, so the planner reads from the btree where the entries of the key lie. The stretch
-- at the start of the heap keeps the post-filter scan, the others are pushed down; the answers of off, on and force are
-- the same (DESIGN.md, extras item 10).
CREATE EXTENSION pg_sift;

CREATE FUNCTION fk_kind(flt text, mode text DEFAULT 'on') RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	p text := '';
BEGIN
	PERFORM set_config('sift.filter_pushdown', mode, true);
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT id FROM fk WHERE body @@> ''common'' AND (%s) ORDER BY sift.score(ctid) DESC LIMIT 10', flt) LOOP
		p := p || l;
	END LOOP;
	RETURN CASE WHEN p !~ 'SiftScan' THEN 'other' WHEN p ~ 'Bitmap Index Scan' THEN 'pushdown' ELSE 'post-filter' END;
END $$;

CREATE FUNCTION fk_top(flt text, mode text, k int DEFAULT 10) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('sift.filter_pushdown', mode, true);
	EXECUTE format('SELECT string_agg(c::text || '':'' || s::text, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, c, s
		FROM (SELECT ctid AS c, sift.score(ctid) AS s FROM fk WHERE body @@> ''common'' AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s) x) y', flt, k) INTO r;
	RETURN r;
END $$;

-- 24000 rows in 10 groups of 2400 rows in the heap, the label of group c being 'g' || (c * 7 % 10): the labels are not in the
-- heap's order, so the correlation of grp is low. every has 40 values in every page. 'common' is in 1 of 3 rows, with a length
-- and a count of the word that vary so that the scores do too
CREATE TABLE fk (id int PRIMARY KEY, grp text, every text, body text, pad text) WITH (autovacuum_enabled = off);
INSERT INTO fk
SELECT i, 'g' || ((i - 1) / 2400 * 7 % 10), 'e' || i % 40,
	CASE WHEN i % 3 = 0 THEN repeat('common ', 1 + i % 3) ELSE '' END || 'w' || i % 97 || repeat(' filler' || i % 11, 1 + i % 13), repeat('p', 100)
FROM generate_series(1, 24000) i;
CREATE INDEX fk_body ON fk USING sift (body);
CREATE INDEX fk_grp ON fk (grp);
CREATE INDEX fk_every ON fk (every);
ANALYZE fk;
SELECT attname, abs(correlation) < 0.5 AS low_correlation FROM pg_stats WHERE tablename = 'fk' AND attname IN ('grp', 'every') ORDER BY 1;

-- g0 is the first group of the heap, g7 the second, g3 the last and g5 one in the middle
CREATE TABLE fk_filters (flt text PRIMARY KEY, what text);
INSERT INTO fk_filters VALUES
	('grp = ''g0''', '10% in the first stretch of the heap'),
	('grp = ''g7''', '10% in the second stretch'),
	('grp = ''g5''', '10% in a stretch in the middle of the heap'),
	('grp = ''g3''', '10% in the last stretch of the heap'),
	('grp = ''g3'' AND every = ''e7''', 'a late stretch and a spread filter'),
	('every = ''e7''', '2.5% in every page');

-- the plan each filter gets with the planner's costs, and the answers of the three modes
SELECT f.what, fk_kind(f.flt) AS plan,
	fk_top(f.flt, 'off') = fk_top(f.flt, 'on') AND fk_top(f.flt, 'on') = fk_top(f.flt, 'force') AS same_top10,
	fk_top(f.flt, 'off', 100000) = fk_top(f.flt, 'force', 100000) AS same_all
FROM fk_filters f ORDER BY f.flt;

-- off and force say so whatever the costs are
SELECT fk_kind('grp = ''g3''', 'off') AS off, fk_kind('grp = ''g0''', 'force') AS forced;

DROP TABLE fk, fk_filters;
DROP FUNCTION fk_kind(text, text), fk_top(text, text, int);
DROP EXTENSION pg_sift;
