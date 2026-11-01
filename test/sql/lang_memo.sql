-- The words a configuration's dictionaries answered are kept by the backend for its next statement: a change of the configuration, of a dictionary, a
-- rename, a drop and a rollback must reach the next statement of the same session (sift._test_syn, a synonym dictionary without a file)
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
CREATE TEXT SEARCH DICTIONARY lm_syn (TEMPLATE = sift._test_syn, MAP = 'kater=katze');
CREATE TEXT SEARCH CONFIGURATION lm (COPY = pg_catalog.german);
ALTER TEXT SEARCH CONFIGURATION lm ALTER MAPPING FOR asciiword, word WITH lm_syn, german_stem;
CREATE FUNCTION terms(w text, cfg text) RETURNS text LANGUAGE sql AS $$ SELECT string_agg(token, ' ') FROM sift.tokenize(w, NULL, cfg) $$;

SELECT terms('Kater Katze Hunde', 'public.lm');
SELECT terms('Kater Katze Hunde', 'public.lm');
-- the dictionary changes
ALTER TEXT SEARCH DICTIONARY lm_syn (MAP = 'kater=hund');
SELECT terms('Kater Katze Hunde', 'public.lm');
-- the mapping changes
ALTER TEXT SEARCH CONFIGURATION lm ALTER MAPPING FOR asciiword, word WITH german_stem;
SELECT terms('Kater Katze Hunde', 'public.lm');
-- a rollback returns the answers of before
BEGIN;
ALTER TEXT SEARCH CONFIGURATION lm ALTER MAPPING FOR asciiword, word WITH lm_syn, german_stem;
SELECT terms('Kater Katze Hunde', 'public.lm');
ROLLBACK;
SELECT terms('Kater Katze Hunde', 'public.lm');
-- the name moves, and a configuration of the old name is another one
ALTER TEXT SEARCH CONFIGURATION lm RENAME TO lm2;
SELECT terms('Kater Katze Hunde', 'public.lm2');
SELECT terms('Kater', 'public.lm');
CREATE TEXT SEARCH CONFIGURATION lm (COPY = pg_catalog.german);
ALTER TEXT SEARCH CONFIGURATION lm ALTER MAPPING FOR asciiword, word WITH lm_syn, german_stem;
SELECT terms('Kater Katze Hunde', 'public.lm'), terms('Kater Katze Hunde', 'public.lm2');
-- two configurations at once, each with its own words
SELECT terms('Kater', 'public.lm'), terms('Kater', 'public.lm2'), terms('Kater', 'public.lm'), terms('Kater', 'public.lm2');

-- the memo is bounded: 200,000 distinct words in each of four configurations stay within TS_MEMOS x (2 MB of entries + 1 MB of table) and its context is a backend cache
CREATE FUNCTION many(cfg text) RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM sift.tokenize((SELECT string_agg('qw' || g || 'x', ' ') FROM generate_series(1, 200000) g), NULL, cfg) $$;
SELECT many('german'), many('french'), many('spanish'), many('russian');
SELECT many('german'), many('pg_catalog.english');
SELECT name, total_bytes < 16 * 1024 * 1024 AS within_cap FROM pg_backend_memory_contexts WHERE name LIKE 'sift%stem memos';

DROP FUNCTION many(text), terms(text, text);
DROP TEXT SEARCH CONFIGURATION lm, lm2;
DROP TEXT SEARCH DICTIONARY lm_syn;
DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift;
