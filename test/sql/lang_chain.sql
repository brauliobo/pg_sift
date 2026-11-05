-- a configuration whose word mapping chains dictionaries: the first that knows the word answers, one that does not leaves it to
-- the next, an answer flagged as a filter is the next one's input (sift._test_syn, a synonym dictionary without a file)
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
CREATE TEXT SEARCH DICTIONARY lang_syn (TEMPLATE = sift._test_syn, MAP = 'kater=katze,hunde=hund');
CREATE TEXT SEARCH DICTIONARY lang_synf (TEMPLATE = sift._test_syn, MAP = 'kater=katzen', FILTER = true);
CREATE TEXT SEARCH DICTIONARY lang_stop (TEMPLATE = simple, STOPWORDS = german);
CREATE TEXT SEARCH CONFIGURATION lang_chain (COPY = pg_catalog.german);
ALTER TEXT SEARCH CONFIGURATION lang_chain ALTER MAPPING FOR asciiword, word WITH lang_syn, german_stem;
CREATE TEXT SEARCH CONFIGURATION lang_chain_f (COPY = pg_catalog.german);
ALTER TEXT SEARCH CONFIGURATION lang_chain_f ALTER MAPPING FOR asciiword, word WITH lang_synf, german_stem;
CREATE TEXT SEARCH CONFIGURATION lang_chain_s (COPY = pg_catalog.german);
ALTER TEXT SEARCH CONFIGURATION lang_chain_s ALTER MAPPING FOR asciiword, word WITH lang_stop, german_stem;

CREATE TABLE cw (cfg text, w text);
INSERT INTO cw SELECT c, unnest(string_to_array('Kater Katze Katzen Hunde Hund Häuser und der Wetter über', ' '))
FROM unnest(ARRAY['public.lang_chain', 'public.lang_chain_f', 'public.lang_chain_s']) c;
SELECT cfg, count(*) AS words, count(*) FILTER (WHERE tsv IS NULL) AS dropped,
	count(*) FILTER (WHERE terms IS DISTINCT FROM tsv) AS differ
FROM (SELECT cfg, w,
		(SELECT array_agg(token ORDER BY token) FROM sift.tokenize(w, NULL, cfg)) AS terms,
		(SELECT array_agg(l ORDER BY l) FROM unnest(tsvector_to_array(to_tsvector(cfg::regconfig, w))) l) AS tsv
	FROM cw) x GROUP BY cfg ORDER BY cfg;
SELECT cfg, w, (SELECT string_agg(token, ' ') FROM sift.tokenize(w, NULL, cfg)) AS term FROM cw WHERE w IN ('Kater', 'Hunde', 'Häuser') ORDER BY cfg, w;

CREATE TABLE cd (id int, body text);
INSERT INTO cd VALUES (1, 'der Kater schläft'), (2, 'die Katze schläft'), (3, 'Hunde bellen'), (4, 'ein Hund bellt'), (5, 'Häuser und Katzen');
CREATE TABLE cdf AS SELECT * FROM cd;
CREATE INDEX cd_idx ON cd USING sift (body) WITH (stemmer = 'public.lang_chain');
CREATE INDEX cdf_idx ON cdf USING sift (body) WITH (stemmer = 'public.lang_chain_f');
SET enable_seqscan = off;
SELECT id FROM cd WHERE body @@> 'Kater' ORDER BY id;
SELECT id FROM cd WHERE body @@> 'hunde' ORDER BY id;
SELECT id FROM cd WHERE body @@> '"die Katze"' ORDER BY id;
SELECT id FROM cdf WHERE body @@> 'Kater' ORDER BY id;
SELECT id FROM cdf WHERE body @@> 'katzen' ORDER BY id;
RESET enable_seqscan;
SELECT sift.verify('cd_idx', true), sift.verify('cdf_idx', true);

-- the metapage of an index with a configuration is format 1.5, which a 3.7 library refuses; the one of a plain index is not raised by it
CREATE TABLE cp AS SELECT * FROM cd;
CREATE INDEX cp_idx ON cp USING sift (body);
CHECKPOINT;
SELECT get_byte(pg_read_binary_file(pg_relation_filepath('cd_idx'), 30, 1), 0) AS config_minor,
	get_byte(pg_read_binary_file(pg_relation_filepath('cp_idx'), 30, 1), 0) AS plain_minor;

-- the analyzer hash of the indexes 3.7 writes is the same (none: 2028147818, en: 614992432, the xxh64-style hash of version 2, case, accent and stemmer bytes), so they read as before
CREATE INDEX cp_en ON cp USING sift (body) WITH (stemmer = 'en');
CHECKPOINT;
CREATE FUNCTION meta_hash(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT get_byte(x, 0) + (get_byte(x, 1) << 8) + (get_byte(x, 2) << 16) + (get_byte(x, 3)::bigint << 24) FROM (SELECT pg_read_binary_file(pg_relation_filepath(idx), 40, 4) x) q $$;
SELECT meta_hash('cp_idx') AS none, meta_hash('cp_en') AS en, meta_hash('cd_idx') NOT IN (2028147818, 614992432) AS config_differs;

-- an index promoted without positions (as 3.7.0 did) is healed by its next merge from the heap text, stop word positions included
CREATE TABLE hsrc (id int PRIMARY KEY, body text);
INSERT INTO hsrc SELECT i, (SELECT string_agg((ARRAY['Hund', 'und', 'die', 'Katze', 'schön', 'der', 'Häuser', 'Wetter'])[1 + (i * k + i / k) % 8], ' ' ORDER BY k) FROM generate_series(1, 9) k)
FROM generate_series(1, 1260) i;
CREATE TABLE hfresh AS SELECT * FROM hsrc;
CREATE TABLE hdamaged AS SELECT * FROM hsrc WHERE id <= 400;
ALTER TABLE hdamaged ADD PRIMARY KEY (id);
CREATE INDEX hfresh_idx ON hfresh USING sift (body) WITH (stemmer = 'german');
CREATE INDEX hdamaged_idx ON hdamaged USING sift (body) WITH (stemmer = 'german');
SET sift.async_maintenance = off;
SET sift.test_no_heap_positions = on;
INSERT INTO hdamaged SELECT * FROM hsrc WHERE id > 400 AND id <= 800;
SELECT sift.promote('hdamaged_idx');
INSERT INTO hdamaged SELECT * FROM hsrc WHERE id > 800 AND id <= 1200;
SELECT sift.promote('hdamaged_idx');
SELECT sift.merge('hdamaged_idx', true) >= 0 AS merged;
RESET sift.test_no_heap_positions;
-- the next merge finds the segment without positions
INSERT INTO hdamaged SELECT * FROM hsrc WHERE id > 1200;
SELECT sift.promote('hdamaged_idx');
SELECT sift.merge('hdamaged_idx', true) >= 0 AS healed;
SELECT count(*) AS phrases, count(*) FILTER (WHERE (SELECT array_agg(id ORDER BY id) FROM hdamaged WHERE body @@> q) IS DISTINCT FROM (SELECT array_agg(id ORDER BY id) FROM hfresh WHERE body @@> q)) AS differ
FROM unnest(ARRAY['"Hund und Katze"', '"Katze die Häuser"', '"Hund Katze"', '"schön der Wetter"', '"die Katze schön"']) q;
SELECT count(*) AS phrases, count(*) FILTER (WHERE (SELECT count(*) FROM hdamaged WHERE body @@> q) <>
	(SELECT count(*) FROM hdamaged WHERE to_tsvector('german', body) @@ phraseto_tsquery('german', trim(both '"' from q)))) AS differ_from_pg
FROM unnest(ARRAY['"Hund und Katze"', '"Katze die Häuser"', '"Hund Katze"', '"schön der Wetter"', '"die Katze schön"']) q;
SELECT sum(bytes) BETWEEN 0.97 * (SELECT sum(bytes) FROM sift.segments('hfresh_idx') WHERE kind = 'immutable')
	AND 1.03 * (SELECT sum(bytes) FROM sift.segments('hfresh_idx') WHERE kind = 'immutable') AS healed_size FROM sift.segments('hdamaged_idx') WHERE kind = 'immutable';
SELECT sift.verify('hdamaged_idx', true);

DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
