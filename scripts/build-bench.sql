-- Corpora of scripts/build-bench.sh (the same documents for the same :rows, whatever the build):
--   art   :rows documents of 300 words drawn from a skewed vocabulary of 200000
--   hotc  10 x :rows documents of 8 words: one term in 9 of 10 documents, the others skewed over 5000 words
--   uniq  10 x :rows documents of 5 words, 2 of them a code unique to the document (a dictionary of 2 terms a document)
CREATE EXTENSION IF NOT EXISTS pg_sift;
DROP TABLE IF EXISTS art, hotc, uniq;
SELECT setseed(0.42);
CREATE TABLE art (id serial, body text) WITH (parallel_workers = 2);
INSERT INTO art (body)
SELECT (SELECT string_agg('w' || floor(power(random(), 3) * 200000)::int, ' ') FROM generate_series(1, 300))
FROM generate_series(1, :rows) i;
CREATE TABLE hotc (id serial, body text) WITH (parallel_workers = 2);
INSERT INTO hotc (body)
SELECT CASE WHEN i % 10 < 9 THEN 'hot ' ELSE '' END ||
       (SELECT string_agg('w' || floor(power(random(), 2) * 5000)::int, ' ') FROM generate_series(1, 7))
FROM generate_series(1, :rows * 10) i;
CREATE TABLE uniq (id serial, body text) WITH (parallel_workers = 2);
INSERT INTO uniq (body)
SELECT 'k' || (i % 50) || ' g' || (i % 3000) || ' c' || i || 'x' || (i::bigint * 7919 % 1000003) || ' w' || (i % 997)
FROM generate_series(1, :rows * 10) i;
VACUUM ANALYZE art, hotc, uniq;
