-- A term lookup in a dictionary whose terms share their first 8 bytes (fixed-width ids, 'ord-0000' + 8 digits) finds
-- its block by bisection, not by reading every block of the run (sift._test_read_stats counts the blocks read).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE ri (id int PRIMARY KEY, ref text);
INSERT INTO ri SELECT i, 'ord-0000' || lpad(i::text, 8, '0') FROM generate_series(1, 40000) i;
CREATE INDEX ri_idx ON ri USING sift (ref) WITH (analyzer = keyword);
ANALYZE ri;

-- every id of a sample is found, and what is not an id is not
SELECT count(*) AS found, count(*) FILTER (WHERE (SELECT count(*) FROM ri r WHERE r.ref @@> t.ref) = 1) AS found_once
FROM (SELECT ref FROM ri WHERE id % 97 = 0) t;
SELECT count(*) AS absent_found FROM (SELECT 'ord-0000' || lpad(i::text, 8, '0') AS ref FROM generate_series(40001, 40050) i) t
WHERE EXISTS (SELECT 1 FROM ri r WHERE r.ref @@> t.ref);
SELECT count(*) AS before_first FROM ri WHERE ref @@> 'ord-00000000-';
SELECT count(*) AS after_last FROM ri WHERE ref @@> 'ord-0000zzzzzzzz';

-- 400 lookups read about log2(blocks) blocks each: far fewer than the run of about 1250 blocks has
SELECT count(*) AS reset FROM sift._test_read_stats(true);
SELECT count(*) AS lookups FROM (SELECT ref FROM ri WHERE id % 100 = 7) t WHERE (SELECT count(*) FROM ri r WHERE r.ref @@> t.ref) = 1;
SELECT value AS probes FROM sift._test_read_stats() WHERE key = 'dict_block_probes' \gset
SELECT :probes > 0 AS probed, :probes < 400 * 16 AS bisected;

DROP TABLE ri;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
