-- A fuzzy leaf scans the dictionary under its first character and seeks past the terms that start with a prefix more than k edits from
-- every prefix of the query: the terms it finds are the ones the per-term matcher accepts (a heap scan evaluates that matcher on the
-- documents), and it reads a small part of the blocks of that range (sift._test_read_stats counts them).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE FUNCTION hits(tbl regclass, query text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %s WHERE body @@> $1', tbl) INTO n USING query;
	RETURN n::text;
END $$;
CREATE FUNCTION hits_seq(tbl regclass, query text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %s WHERE body @@> $1', tbl) INTO n USING query;
	RETURN n::text;
END $$;

-- words of 3 to 9 letters over sixteen letters, so that many are one or two edits apart, a tenth with two accented letters
CREATE TABLE fs (id int, body text);
INSERT INTO fs SELECT i, CASE WHEN i % 10 = 0 THEN translate(w, 'ab', U&'\00e9\00df') ELSE w END
FROM (SELECT i, translate(substr(md5(i::text), 1, 3 + i % 7), '0123456789', 'ghijklmnop') AS w FROM generate_series(1, 6000) i) t;
CREATE INDEX fs_idx ON fs USING sift (body);
ANALYZE fs;

-- every query: a word, the word with a letter deleted, replaced, inserted, a prefix of it and a word of no table, at distance 1 and 2
CREATE TEMP TABLE fq AS
SELECT q || '~' || k AS q
FROM (SELECT w, unnest(ARRAY[w, left(w, 2) || substr(w, 4), left(w, 1) || 'z' || substr(w, 3), left(w, 3) || 'q' || substr(w, 4),
                             left(w, 4), md5(w)::text]) AS q
      FROM (SELECT body AS w FROM fs WHERE id % 40 = 3 AND body ~ '^[a-p]+$') t) u, generate_series(1, 2) k
WHERE length(q) >= 2 AND q ~ '^[a-pz]+$';
SELECT count(*) > 500 AS many_queries FROM fq;
SELECT count(*) AS queries, count(*) FILTER (WHERE hits('fs', q) <> hits_seq('fs', q)) AS different, count(*) FILTER (WHERE hits('fs', q)::int > 1) > 300 AS not_empty FROM fq;
-- the accented words, the first character fixed, a query that is one character
SELECT hits('fs', w || '~1') = hits_seq('fs', w || '~1') AS same, hits('fs', w || '~1')::int > 0 AS found
FROM (SELECT body AS w FROM fs WHERE id % 10 = 0 LIMIT 5) t;
SELECT hits('fs', 'z~1') = hits_seq('fs', 'z~1') AS single_letter;

-- 60,000 words of 8 to 12 letters over eight letters: the words share long prefixes, as those of a language do, and a prefix more than one edit from the
-- query's holds a sixteenth to a thousandth of the dictionary. Without the seeks, a query decodes every block under its first letter (about 235, twice: expansion and count).
CREATE TABLE fl (id int, body text);
INSERT INTO fl SELECT i, translate(substr(md5(i::text), 1, 8 + i % 5), '0123456789abcdef', 'aabbccddeeffgghh') FROM generate_series(1, 60000) i;
CREATE INDEX fl_idx ON fl USING sift (body);
ANALYZE fl;
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT count(*) AS lookups, sum(n) >= 20 AS found
FROM (SELECT hits('fl', left(t.body, 2) || substr(t.body, 4) || '~1')::int AS n FROM fl t WHERE id % 3000 = 5) q;
SELECT value AS blocks FROM sift._test_read_stats() WHERE key = 'dict_scan_blocks' \gset
SELECT :blocks > 0 AS scanned, :blocks < 20 * 350 AS skipped;
SELECT count(*) AS different FROM (SELECT left(body, 2) || substr(body, 4) || '~1' AS q FROM fl WHERE id % 6000 = 7) t
WHERE hits('fl', t.q) <> hits_seq('fl', t.q);

DROP FUNCTION hits(regclass, text), hits_seq(regclass, text);
DROP TABLE fs, fl;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
