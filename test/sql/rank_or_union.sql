-- The candidates of a ranked OR are the union of the groups its terms have in the block being scored: the ids a search returns, its k best scores
-- and the k best scores of one that keeps every match are the ones computed from the text, for terms that sit in few, many and every superblock,
-- before and after deletes and VACUUM.
CREATE EXTENSION pg_sift;

-- 200,000 rows on about 2,100 heap pages (8 superblocks): 'common' in every row, seven terms in 28,000, 500 terms in 400, 'rare' in 2,000, 'x y' in 15,000,
-- 'early' in the first 300 rows, 'mid' in 401 rows of one superblock, 'late' in the last 5,000
CREATE TABLE src AS
SELECT g AS id, g % 10 AS counter_a, concat_ws(' ', 'common', 'w' || g % 7, 'r' || g % 500, CASE WHEN g % 100 = 0 THEN 'rare' END, CASE WHEN g % 13 = 0 THEN 'x y' END,
	CASE WHEN g < 300 THEN 'early' END, CASE WHEN g BETWEEN 100000 AND 100400 THEN 'mid' END, CASE WHEN g > 195000 THEN 'late' END, repeat('f ', g % 5)) AS body
FROM generate_series(1, 200000) g;
SET sift.grouped_min_df = 20000;
CREATE INDEX src_idx ON src USING sift (body);

CREATE TABLE tok AS SELECT DISTINCT id, t FROM src, unnest(string_to_array(body, ' ')) t WHERE t <> '';
CREATE INDEX tok_t ON tok (t, id);
CREATE TABLE words (t text);
INSERT INTO words VALUES ('w0'), ('w3'), ('w6'), ('rare'), ('x'), ('r1'), ('r250'), ('early'), ('mid'), ('late');
-- every pair of words, three words, and the dense words with the sparse ones
CREATE TABLE cases AS
SELECT a.t || ' OR ' || b.t AS q, ARRAY[a.t, b.t] AS w FROM words a JOIN words b ON a.t < b.t
UNION ALL SELECT a.t || ' OR ' || b.t || ' OR ' || c.t, ARRAY[a.t, b.t, c.t] FROM words a JOIN words b ON a.t < b.t JOIN words c ON b.t < c.t WHERE a.t IN ('w3', 'mid', 'r1') AND c.t IN ('late', 'rare', 'early', 'x')
UNION ALL SELECT 'common OR ' || t, ARRAY['common', t] FROM words WHERE t IN ('late', 'mid', 'early', 'r1');

CREATE FUNCTION compare() RETURNS TABLE (cases bigint, same_ids bigint, same_top10 bigint, same_top100 bigint) LANGUAGE plpgsql AS $f$
DECLARE
	c cases;
	a text; b text; ta text; tb text;
	i bigint = 0; k bigint = 0; m bigint = 0; n bigint = 0;
BEGIN
	FOR c IN SELECT * FROM cases LOOP
		n = n + 1;
		EXECUTE format('SELECT md5(string_agg(id::text, '','' ORDER BY id)) FROM (SELECT id FROM src WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 1000000) a', c.q) INTO a;
		EXECUTE format('SELECT md5(string_agg(id::text, '','' ORDER BY id)) FROM (SELECT DISTINCT id FROM tok JOIN src USING (id) WHERE t = ANY (%L)) r', c.w) INTO b;
		IF a IS NOT DISTINCT FROM b THEN i = i + 1; END IF;
		FOREACH tb IN ARRAY ARRAY['10', '100'] LOOP
			EXECUTE format('SELECT md5(string_agg(round(s::numeric, 6)::text, '','' ORDER BY s DESC)) FROM (SELECT sift.score(ctid) AS s FROM src WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT %s) r', c.q, tb) INTO ta;
			EXECUTE format('SELECT md5(string_agg(round(s::numeric, 6)::text, '','' ORDER BY s DESC)) FROM (SELECT s FROM (SELECT sift.score(ctid) AS s FROM src WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 1000000) a ORDER BY s DESC LIMIT %s) r', c.q, tb) INTO b;
			IF ta IS NOT DISTINCT FROM b THEN IF tb = '10' THEN k = k + 1; ELSE m = m + 1; END IF; END IF;
		END LOOP;
	END LOOP;
	RETURN QUERY SELECT n, i, k, m;
END $f$;
SELECT * FROM compare();

DELETE FROM src WHERE id % 4 = 1;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) src;
DELETE FROM tok WHERE id % 4 = 1;
SELECT * FROM compare();

DROP FUNCTION compare();
DROP TABLE cases;
DROP TABLE words;
DROP TABLE tok;
DROP TABLE src;
DROP EXTENSION pg_sift;
