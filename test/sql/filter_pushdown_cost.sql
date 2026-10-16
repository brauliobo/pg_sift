-- Plan choice of the filter-mask pushdown (DESIGN.md, extras item 10). The ranker visits the heap in order and
-- verifies every match until it holds k rows that pass the quals, so a filter after the ranker is cheap when the
-- rows that pass are spread over the heap and expensive when they are clustered away from its start. The planner
-- prices that from the filter column's statistics; the answers of off, on and force must be the same.
CREATE EXTENSION pg_sift;

CREATE FUNCTION fc_kind(flt text, mode text, k int DEFAULT 10) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	p text := '';
BEGIN
	PERFORM set_config('sift.filter_pushdown', mode, true);
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT id FROM fc WHERE body @@> ''common'' AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s', flt, k) LOOP
		p := p || l;
	END LOOP;
	RETURN CASE WHEN p !~ 'SiftScan' THEN 'other' WHEN p ~ 'Bitmap Index Scan' THEN 'pushdown' ELSE 'post-filter' END;
END $$;

CREATE FUNCTION fc_top(flt text, mode text, k int DEFAULT 10) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('sift.filter_pushdown', mode, true);
	EXECUTE format('SELECT string_agg(c::text || '':'' || s::text, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, c, s
		FROM (SELECT ctid AS c, sift.score(ctid) AS s FROM fc WHERE body @@> ''common'' AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s) x) y', flt, k) INTO r;
	RETURN r;
END $$;

-- 24000 rows: id is the heap order, scat a permutation of it (no correlation with the heap), grp 40 groups; 'common' is in
-- 3 of 5 rows, with a length and a count of the word that vary so that the scores do too
CREATE TABLE fc (id int PRIMARY KEY, scat int, grp int, body text, pad text) WITH (autovacuum_enabled = off);
INSERT INTO fc
SELECT i, (i * 7919) % 24001, i % 40,
	CASE WHEN i % 5 < 3 THEN repeat('common ', 1 + i % 3) ELSE '' END || 'w' || i % 97 || repeat(' filler' || i % 11, 1 + i % 13), repeat('p', 100)
FROM generate_series(1, 24000) i;
CREATE INDEX fc_body ON fc USING sift (body);
CREATE UNIQUE INDEX fc_scat ON fc (scat);
CREATE INDEX fc_grp ON fc (grp);
ANALYZE fc;
SELECT attname, round(correlation::numeric) AS correlation FROM pg_stats WHERE tablename = 'fc' AND attname IN ('id', 'scat') ORDER BY 1;

CREATE TABLE fc_filters (flt text PRIMARY KEY, what text);
INSERT INTO fc_filters VALUES
	('id BETWEEN 20401 AND 21600', '5% at the end of the heap'),
	('id BETWEEN 9601 AND 10800', '5% in the middle of the heap'),
	('id BETWEEN 1 AND 1200', '5% at the start of the heap'),
	('id >= 7201', 'the last 70%'),
	('id = ANY(ARRAY[20401, 20402, 20403, 20404, 20405, 20406, 20407, 20408, 20409, 20410])', 'a few ids at the end of the heap'),
	('scat BETWEEN 1 AND 1200', '5% spread over the heap'),
	('scat BETWEEN 1 AND 7200', '30% spread over the heap'),
	('grp = 7', '2.5% in every page'),
	('id BETWEEN 20401 AND 21600 AND grp < 20', 'a clustered range and a spread filter');
-- id lists: array elements on one leaf page share a descent, the elements of a list over the heap do not
INSERT INTO fc_filters SELECT format('scat = ANY(ARRAY[%s])', string_agg((i * 79)::text, ',')), 'a list of 300 ids spread over the heap' FROM generate_series(1, 300) i;
INSERT INTO fc_filters SELECT format('id = ANY(ARRAY[%s])', string_agg((i * 3)::text, ',')), 'a list of 300 ids at the start of the heap' FROM generate_series(1, 300) i;
INSERT INTO fc_filters SELECT format('id = ANY(ARRAY[%s])', string_agg((24000 - i * 3)::text, ',')), 'a list of 300 ids at the end of the heap' FROM generate_series(1, 300) i;

-- the plan each filter gets with the planner's costs, and the answers of the three modes
SELECT f.what, fc_kind(f.flt, 'on') AS plan,
	fc_top(f.flt, 'off') = fc_top(f.flt, 'on') AND fc_top(f.flt, 'on') = fc_top(f.flt, 'force') AS same_top10,
	fc_top(f.flt, 'off', 100000) = fc_top(f.flt, 'force', 100000) AS same_all
FROM fc_filters f ORDER BY f.flt;

-- off and force say so whatever the costs are
SELECT fc_kind('id BETWEEN 20401 AND 21600', 'off') AS off, fc_kind('scat BETWEEN 1 AND 7200', 'force') AS forced;

DROP TABLE fc, fc_filters;
DROP FUNCTION fc_kind(text, text, int), fc_top(text, text, int);
DROP EXTENSION pg_sift;
