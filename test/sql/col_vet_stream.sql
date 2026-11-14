-- A unit with dead rows checks the rows it selects against the heap, and from its eighth page on asks the heap for the pages through a read stream (combined reads
-- ahead of the check) where it read each page as it came. The rows left must be those the page-at-a-time check leaves: after a DELETE and an UPDATE with no VACUUM, for
-- units cut by a key (a page is met by several units, the second visit takes the cached mask) and units in heap order, for a vet of a few pages (below the stream) and of
-- all of them.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SELECT setseed(0.42);

CREATE TABLE vs_keyed (id int PRIMARY KEY, g int NOT NULL, c int NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = false);
INSERT INTO vs_keyed SELECT i, (i::bigint * 7919) % 50, (i::bigint * 104729) % 1000, (random() * 100)::int, md5(i::text) FROM generate_series(1, 120000) i;
CREATE TABLE vs_heap (LIKE vs_keyed INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO vs_heap SELECT * FROM vs_keyed;
CREATE INDEX vs_keyed_col ON vs_keyed USING sift_col (g, c, v) WITH (key_cut = 'c', rollup_columns = 'off');
CREATE INDEX vs_heap_col ON vs_heap USING sift_col (g, c, v) WITH (key_cut = 'off', rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) vs_keyed;
VACUUM (FREEZE, ANALYZE) vs_heap;
DELETE FROM vs_keyed WHERE id % 11 = 3;
UPDATE vs_keyed SET v = v + 1 WHERE id % 13 = 0;
DELETE FROM vs_heap WHERE id % 11 = 3;
UPDATE vs_heap SET v = v + 1 WHERE id % 13 = 0;

CREATE FUNCTION vs_same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_bitmapscan = off;
	EXECUTE 'SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (' || q || ') t' INTO b;
	RETURN a = b;
END $$;

SELECT t, vs_same(replace(q, '@', t)) AS same_as_off
FROM (VALUES ('vs_keyed'), ('vs_heap')) tt (t),
     (VALUES ('SELECT g, count(*), sum(v), min(v), max(v) FROM @ GROUP BY g'),
             ('SELECT count(*), sum(v) FROM @'),
             ('SELECT g, sum(v) FROM @ WHERE c < 100 GROUP BY g'),
             ('SELECT sum(v) FROM @ WHERE c = 7 AND g < 3'),
             ('SELECT c, count(*) FROM @ WHERE g = 5 GROUP BY c')) qq (q)
ORDER BY t, q;

-- a second read of the same unit after the pages' visibility is cached
SELECT vs_same('SELECT g, count(*), sum(v) FROM vs_keyed GROUP BY g') AS same_again;

DROP FUNCTION vs_same(text);
DROP TABLE vs_keyed, vs_heap;
DROP EXTENSION pg_sift CASCADE;
