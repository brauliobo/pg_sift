-- phrases inside a larger query: the documents the bitmaps decide alone (an OR branch without the
-- phrase, a NOT of a superset) are not rechecked against the text, the others are, and every path
-- (SiftCount, SiftScan, Bitmap Heap Scan) returns what a sequential evaluation does. Run without word
-- positions (the heap decides every phrase) and with them (the heap decides only the documents the
-- positions do not cover: write buffers and promoted segments, a negated phrase)
CREATE EXTENSION pg_sift;

CREATE TABLE s (id int PRIMARY KEY, body text, grp int);
INSERT INTO s SELECT i, (ARRAY['quick brown fox', 'fox quick', 'lazy dog', 'the quick red fox', 'brown dog quick',
	'quick fox jumps'])[1 + i % 6] || ' w' || i % 5, i % 4 FROM generate_series(1, 1200) i;
CREATE INDEX s_idx ON s USING sift (body) WITH (positions = off);
VACUUM ANALYZE s;

CREATE FUNCTION via_count(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM s WHERE body @@> $1' INTO n USING q;
	RETURN n;
END $$;
CREATE FUNCTION via_rows(q text) RETURNS bigint LANGUAGE plpgsql
	SET sift.enable_count = off SET enable_bitmapscan = off AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE 'SELECT count(id) FROM s WHERE body @@> $1' INTO n USING q;
	RETURN n;
END $$;
CREATE FUNCTION via_bitmap(q text) RETURNS bigint LANGUAGE plpgsql
	SET sift.enable_count = off SET sift.enable_scan = off SET enable_seqscan = off SET enable_indexscan = off AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE 'SELECT count(id) FROM s WHERE body @@> $1' INTO n USING q;
	RETURN n;
END $$;
CREATE FUNCTION via_seq(q text) RETURNS bigint LANGUAGE sql
	SET sift.enable_count = off SET sift.enable_scan = off SET enable_bitmapscan = off SET enable_indexscan = off
	RETURN (SELECT count(*) FROM s WHERE body @@> q);

-- documents the SiftCount node reads and rechecks
CREATE FUNCTION rechecked(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM s WHERE body @@> '
		|| quote_literal(q) LOOP
		IF l ~ '(\d+) candidates' THEN
			RETURN substring(l from '(\d+) candidates');
		END IF;
	END LOOP;
	RETURN 0;
END $$;

CREATE FUNCTION report() RETURNS TABLE (q text, matches bigint, same boolean, rechecked bigint) LANGUAGE sql AS $$
	SELECT q, via_count(q), via_count(q) = via_seq(q) AND via_rows(q) = via_seq(q) AND via_bitmap(q) = via_seq(q),
		rechecked(q)
	FROM (VALUES ('"quick fox"'), ('dog OR "quick fox"'), ('quick OR "quick fox"'), ('"quick fox" OR "fox quick"'),
		('quick AND NOT "quick fox"'), ('fox AND NOT "quick fox"'), ('(dog OR "quick fox") AND w1'),
		('"quick fox" AND w1'), ('(brown OR "quick fox") AND NOT ("quick fox" AND w2)'), ('dog OR ("fox quick" AND w3)'),
		('quick fox'), ('fox NOT "quick red"')) v(q)
$$;

SELECT * FROM report();

-- dead rows keep their pages out of the visibility map: the sure documents are checked in the heap
DELETE FROM s WHERE id % 7 = 0;
SELECT * FROM report();

-- inserts after the build live in the write buffer, whose matches are all rechecked
INSERT INTO s SELECT 2000 + i, (ARRAY['quick fox', 'fox quick', 'dog'])[1 + i % 3], 0 FROM generate_series(1, 90) i;
SELECT * FROM report();

-- vacuum clears the dead documents' bits (dirty superblocks), merge folds the write buffer in
VACUUM s;
SELECT * FROM report();
SELECT sift.promote('s_idx') IS NOT NULL AS promoted, sift.merge('s_idx') IS NOT NULL AS merged;
SELECT * FROM report();

-- the other quals of the statement and the parallel scan see the same rows
SELECT count(*) FROM s WHERE body @@> 'dog OR "quick fox"' AND grp = 1;
SET sift.parallel_scan = on;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM report();
RESET sift.parallel_scan;

-- with positions: the segments of the build decide their phrases, only the negated ones and the
-- documents beyond them are rechecked
DROP INDEX s_idx;
CREATE INDEX s_idx ON s USING sift (body) WITH (positions = on);
VACUUM ANALYZE s;
SELECT * FROM report();

DELETE FROM s WHERE id % 11 = 0;
SELECT * FROM report();

-- the write buffer has no positions, nor does the segment promoted from it: the index mixes both
INSERT INTO s SELECT 3000 + i, (ARRAY['quick fox', 'fox quick', 'dog'])[1 + i % 3], 0 FROM generate_series(1, 90) i;
SELECT * FROM report();
VACUUM s;
SELECT sift.promote('s_idx') IS NOT NULL AS promoted;
SELECT * FROM report();

-- merge
SELECT sift.merge('s_idx') IS NOT NULL AS merged;
SELECT * FROM report();

SET sift.parallel_scan = on;
SELECT * FROM report();
RESET sift.parallel_scan;

DROP FUNCTION report(), via_count(text), via_rows(text), via_bitmap(text), via_seq(text), rechecked(text);
DROP TABLE s;
DROP EXTENSION pg_sift;
