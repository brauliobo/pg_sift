-- The candidates of a ranked OR are the union of the groups its terms have in the block being scored: the ids a search returns, its k best scores
-- and the k best scores of one that keeps every match are the ones computed from the text, for terms that sit in few, many and every superblock,
-- before and after deletes and VACUUM.
CREATE EXTENSION pg_sift;

-- Two tables with the same terms: 20,000 rows of 900 bytes on about 2,100 heap pages (8 superblocks, as 200,000 narrow rows have; the groups of its pages have a mask of a few bytes) and
-- 20,000 narrow rows on about 200 pages (the groups have a mask of 8 bytes or more, which the union reads in words). 'common' in every row, seven terms in 2,800, 500 terms in 40,
-- 'rare' in 200, 'x y' in 1,500, 'early' in the first 30 rows, 'mid' in 41 rows, 'late' in the last 500; the terms with 2,000 rows or more are posting groups, the others lists.
CREATE FUNCTION mk(tbl text, pad int) RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
	EXECUTE format('CREATE TABLE %I AS SELECT g AS id, g %% 10 AS counter_a, concat_ws('' '', ''common'', ''w'' || g %% 7, ''r'' || g %% 500, CASE WHEN g %% 100 = 0 THEN ''rare'' END, CASE WHEN g %% 13 = 0 THEN ''x y'' END,
		CASE WHEN g < 30 THEN ''early'' END, CASE WHEN g BETWEEN 10000 AND 10040 THEN ''mid'' END, CASE WHEN g > 19500 THEN ''late'' END, repeat(''f '', g %% 5)) AS body, repeat(''p'', %s) AS pad
		FROM generate_series(1, 20000) g', tbl, pad);
	EXECUTE format('ALTER TABLE %I ALTER COLUMN pad SET STORAGE PLAIN', tbl);
	EXECUTE format('CREATE TABLE %I AS SELECT DISTINCT id, t FROM %I, unnest(string_to_array(body, '' '')) t WHERE t <> ''''', tbl || '_tok', tbl);
	EXECUTE format('CREATE INDEX ON %I (t, id)', tbl || '_tok');
END $f$;
SET sift.grouped_min_df = 2000;
SELECT mk('wide', 720), mk('narrow', 0);
CREATE INDEX wide_idx ON wide USING sift (body);
CREATE INDEX narrow_idx ON narrow USING sift (body);
SELECT pg_relation_size('wide') / current_setting('block_size')::int BETWEEN 1800 AND 2300 AS eight_superblocks, pg_relation_size('narrow') / current_setting('block_size')::int < 256 AS one_superblock;

CREATE TABLE words (t text);
INSERT INTO words VALUES ('w0'), ('w3'), ('w6'), ('rare'), ('x'), ('r1'), ('r250'), ('early'), ('mid'), ('late');
-- every pair of words, three words, and the dense words with the sparse ones
CREATE TABLE cases AS
SELECT a.t || ' OR ' || b.t AS q, ARRAY[a.t, b.t] AS w FROM words a JOIN words b ON a.t < b.t
UNION ALL SELECT a.t || ' OR ' || b.t || ' OR ' || c.t, ARRAY[a.t, b.t, c.t] FROM words a JOIN words b ON a.t < b.t JOIN words c ON b.t < c.t WHERE a.t IN ('w3', 'mid', 'r1') AND c.t IN ('late', 'rare', 'early', 'x')
UNION ALL SELECT 'common OR ' || t, ARRAY['common', t] FROM words WHERE t IN ('late', 'mid', 'early', 'r1');

CREATE FUNCTION compare(tbl text) RETURNS TABLE (cases bigint, same_ids bigint, same_top10 bigint, same_top100 bigint) LANGUAGE plpgsql AS $f$
DECLARE
	c cases;
	a text; b text; ta text; tb text;
	i bigint = 0; k bigint = 0; m bigint = 0; n bigint = 0;
BEGIN
	FOR c IN SELECT * FROM cases LOOP
		n = n + 1;
		EXECUTE format('SELECT md5(string_agg(id::text, '','' ORDER BY id)) FROM (SELECT id FROM %I WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 1000000) a', tbl, c.q) INTO a;
		EXECUTE format('SELECT md5(string_agg(id::text, '','' ORDER BY id)) FROM (SELECT DISTINCT id FROM %I JOIN %I USING (id) WHERE t = ANY (%L)) r', tbl || '_tok', tbl, c.w) INTO b;
		IF a IS NOT DISTINCT FROM b THEN i = i + 1; END IF;
		FOREACH tb IN ARRAY ARRAY['10', '100'] LOOP
			EXECUTE format('SELECT md5(string_agg(round(s::numeric, 6)::text, '','' ORDER BY s DESC)) FROM (SELECT sift.score(ctid) AS s FROM %I WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT %s) r', tbl, c.q, tb) INTO ta;
			EXECUTE format('SELECT md5(string_agg(round(s::numeric, 6)::text, '','' ORDER BY s DESC)) FROM (SELECT s FROM (SELECT sift.score(ctid) AS s FROM %I WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 1000000) a ORDER BY s DESC LIMIT %s) r', tbl, c.q, tb) INTO b;
			IF ta IS NOT DISTINCT FROM b THEN IF tb = '10' THEN k = k + 1; ELSE m = m + 1; END IF; END IF;
		END LOOP;
	END LOOP;
	RETURN QUERY SELECT n, i, k, m;
END $f$;
SELECT * FROM compare('wide');
SELECT * FROM compare('narrow');

DELETE FROM wide WHERE id % 4 = 1;
DELETE FROM narrow WHERE id % 4 = 1;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) wide;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) narrow;
DELETE FROM wide_tok WHERE id % 4 = 1;
DELETE FROM narrow_tok WHERE id % 4 = 1;
SELECT * FROM compare('wide');
SELECT * FROM compare('narrow');

DROP FUNCTION compare(text);
DROP FUNCTION mk(text, int);
DROP TABLE cases;
DROP TABLE words;
DROP TABLE wide, wide_tok, narrow, narrow_tok;
DROP EXTENSION pg_sift;
