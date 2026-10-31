-- stemmer = a text search configuration (src/bm25/analyze_ts.c): the words are the ones to_tsvector indexes (the same
-- stems, the same stop words dropped, stop words keeping their position), for German, French, Spanish, Russian, English,
-- 'simple' and a configuration created here; phrases, prefixes, maintenance, INCLUDE columns and builds with workers
-- answer as to_tsvector @@ (plain|phrase)to_tsquery does.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TEXT SEARCH CONFIGURATION lang_de2 (COPY = pg_catalog.german);

CREATE TABLE lw (cfg text, w text);
INSERT INTO lw
SELECT c, unnest(string_to_array(ws, ' ')) FROM (VALUES
	('german', 'Haus Häuser Kinder Kindern laufen gelaufen schönen Wetter gestern Straßen Katze Katzen Hunde und der die das nicht schlecht Zeit Städte Mädchen über Fuß abc12 42 house'),
	('public.lang_de2', 'Haus Häuser Kinder Kindern laufen gelaufen schönen Wetter gestern Straßen und der die das'),
	('french', 'maisons maison enfants courent couru beauté nouvelle météo hier chats chat chiens le la les et pas très être été ête œuvre garçon abc12 42 house'),
	('spanish', 'casas casa niños corren corrido hermosas tiempo ayer gatos gato perros el la los y no muy más está pingüino abc12 42 house'),
	('russian', 'дома домов дети детей бегут бегал красивые погода вчера кошки кошка собаки и не очень ещё сигнал сигнала abc12 42 house'),
	('pg_catalog.english', 'houses running quickly the and of children better went was agreed generously abc12 42'),
	('simple', 'Houses The AND häuser abc12 42')) v(c, ws);

-- the terms of each word equal to_tsvector's
SELECT cfg, count(*) AS words, count(*) FILTER (WHERE tsv IS NULL) AS dropped,
	count(*) FILTER (WHERE terms IS DISTINCT FROM tsv) AS differ
FROM (SELECT cfg, w,
		(SELECT array_agg(token ORDER BY token) FROM sift.tokenize(w, NULL, cfg)) AS terms,
		(SELECT array_agg(l ORDER BY l) FROM unnest(tsvector_to_array(to_tsvector(cfg::regconfig, w))) l) AS tsv
	FROM lw) x GROUP BY cfg ORDER BY cfg;

-- sentences: the terms and their positions (stop words take one) equal to_tsvector's
CREATE TABLE ls (cfg text, s text);
INSERT INTO ls VALUES
	('german', 'die Häuser der alten Stadt sind schön und das Wetter war gestern schlecht'),
	('german', 'der Hund und die Katze'),
	('german', 'und der die das'),
	('german', 'Kinder laufen über die Straßen'),
	('french', 'les maisons de la ville sont très belles et le temps est beau'),
	('french', 'le chat et le chien'),
	('spanish', 'las casas de la ciudad son muy hermosas y el tiempo es bueno'),
	('spanish', 'el gato y el perro'),
	('russian', 'дома в городе очень красивые и погода хорошая'),
	('russian', 'кошка и собака'),
	('pg_catalog.english', 'the houses of the old town are very beautiful and the weather was good'),
	('simple', 'The Houses of the Town');
SELECT cfg, count(*) AS sentences,
	count(*) FILTER (WHERE (SELECT count(*) FROM (
		(SELECT token, pos + 1 FROM sift.tokenize(s, NULL, cfg) EXCEPT SELECT lexeme, unnest(positions) FROM unnest(to_tsvector(cfg::regconfig, s)))
		UNION ALL
		(SELECT lexeme, unnest(positions) FROM unnest(to_tsvector(cfg::regconfig, s)) EXCEPT SELECT token, pos + 1 FROM sift.tokenize(s, NULL, cfg))) d) > 0) AS differ
FROM ls GROUP BY cfg ORDER BY cfg;

-- a configuration is a stemmer setting like the others: names are checked, other analyzers refuse it
CREATE TABLE le (id int, body text);
CREATE INDEX le_bad ON le USING sift (body) WITH (stemmer = 'no_such_config');
CREATE TEXT SEARCH CONFIGURATION lang_empty (PARSER = default);
CREATE INDEX le_empty ON le USING sift (body) WITH (stemmer = 'public.lang_empty');
CREATE INDEX le_kw ON le USING sift (body) WITH (stemmer = 'german', analyzer = keyword);
DROP TABLE le;

-- documents from word lists, per language
CREATE TABLE lang_words (cfg text, words text[]);
INSERT INTO lang_words VALUES
	('german', ARRAY['Haus', 'Häuser', 'Kinder', 'laufen', 'schön', 'Wetter', 'gestern', 'Katze', 'Hund', 'und', 'der', 'die', 'das', 'nicht', 'Straße', 'Zeit']),
	('french', ARRAY['maison', 'maisons', 'enfants', 'courent', 'beauté', 'météo', 'hier', 'chat', 'chien', 'et', 'le', 'la', 'les', 'pas', 'très', 'été']),
	('spanish', ARRAY['casa', 'casas', 'niños', 'corren', 'hermosas', 'tiempo', 'ayer', 'gato', 'perro', 'y', 'el', 'la', 'los', 'no', 'muy', 'más']),
	('russian', ARRAY['дома', 'домов', 'дети', 'бегут', 'красивые', 'погода', 'вчера', 'кошка', 'собака', 'и', 'не', 'очень', 'ещё', 'сигнал', 'сигнала', 'house']);

CREATE TABLE d_german (id int PRIMARY KEY, body text, n int);
CREATE TABLE d_french (id int PRIMARY KEY, body text, n int);
CREATE TABLE d_spanish (id int PRIMARY KEY, body text, n int);
CREATE TABLE d_russian (id int PRIMARY KEY, body text, n int);
-- (the generator: 8 to 10 words per document, picked by arithmetic of the id, so every pair and triple occurs)
CREATE FUNCTION gen_doc(w text[], i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg(w[1 + (i * (k + 3) + (i / (k + 2)) * k + k * k) % array_length(w, 1)], ' ' ORDER BY k)
	FROM generate_series(1, 8 + i % 3) k $$;
INSERT INTO d_german SELECT i, gen_doc(words, i), i % 5 FROM generate_series(1, 600) i, lang_words WHERE cfg = 'german';
INSERT INTO d_french SELECT i, gen_doc(words, i), i % 5 FROM generate_series(1, 600) i, lang_words WHERE cfg = 'french';
INSERT INTO d_spanish SELECT i, gen_doc(words, i), i % 5 FROM generate_series(1, 600) i, lang_words WHERE cfg = 'spanish';
INSERT INTO d_russian SELECT i, gen_doc(words, i), i % 5 FROM generate_series(1, 600) i, lang_words WHERE cfg = 'russian';
CREATE INDEX d_german_idx ON d_german USING sift (body) WITH (stemmer = 'german');
CREATE INDEX d_french_idx ON d_french USING sift (body) WITH (stemmer = 'french');
CREATE INDEX d_spanish_idx ON d_spanish USING sift (body) WITH (stemmer = 'spanish');
CREATE INDEX d_russian_idx ON d_russian USING sift (body) WITH (stemmer = 'russian');
ANALYZE d_german; ANALYZE d_french; ANALYZE d_spanish; ANALYZE d_russian;
SELECT cfg, sift.verify(('d_' || cfg || '_idx')::regclass, true) FROM lang_words ORDER BY cfg;

-- Queries per language: a word, a phrase of two and of three words (stop words inside), a word and a prefix, as plain
-- words of the corpus. The oracle is to_tsvector @@ the query of the same configuration on the same text.
CREATE FUNCTION lang_queries(w text[]) RETURNS TABLE (kind text, q text, pgq text) LANGUAGE sql AS $$
	SELECT 'word', x, x FROM unnest(w) x
	UNION ALL SELECT 'phrase2', w[a] || ' ' || w[b], w[a] || ' ' || w[b]
		FROM generate_series(1, array_length(w, 1)) a, generate_series(1, array_length(w, 1)) b WHERE (a * 7 + b * 3) % 5 = 0
	UNION ALL SELECT 'phrase3', w[a] || ' ' || w[b] || ' ' || w[c], w[a] || ' ' || w[b] || ' ' || w[c]
		FROM generate_series(1, array_length(w, 1)) a, generate_series(1, array_length(w, 1)) b, generate_series(1, array_length(w, 1)) c
		WHERE (a * 5 + b * 3 + c) % 17 = 0 $$;

CREATE FUNCTION lang_check(cfg text, kind text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE sift_n bigint; heap_n bigint; pg_n bigint; tsq tsquery;
BEGIN
	IF kind = 'word' THEN
		tsq := plainto_tsquery(cfg::regconfig, q);
		EXECUTE format('SELECT count(*) FROM d_%s WHERE body @@> %L', cfg, q) INTO sift_n;
	ELSE
		tsq := phraseto_tsquery(cfg::regconfig, q);
		EXECUTE format('SELECT count(*) FROM d_%s WHERE body @@> %L', cfg, '"' || q || '"') INTO sift_n;
	END IF;
	EXECUTE format('SELECT count(*) FROM d_%s WHERE to_tsvector(%L::regconfig, body) @@ %L::tsquery', cfg, cfg, tsq) INTO pg_n;
	RETURN CASE WHEN sift_n = pg_n THEN 0 ELSE 1 END;
END $$;

SELECT l.cfg, q.kind, count(*) AS queries, sum(lang_check(l.cfg, q.kind, q.q)) AS differ
FROM lang_words l, LATERAL lang_queries(l.words) q GROUP BY l.cfg, q.kind ORDER BY l.cfg, q.kind;

-- the same through a scan of the index in a plan that ranks, and through the exact check of the text (sift.match)
SET enable_seqscan = off;
SELECT count(*) AS ranked_differ FROM d_german WHERE body @@> '"Hund und Katze"' AND NOT (to_tsvector('german', body) @@ phraseto_tsquery('german', 'Hund und Katze'));
SELECT id FROM d_german WHERE body @@> '"Hund und Katze"' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
RESET enable_seqscan;
SELECT count(*) AS match_differ FROM d_german
WHERE sift.match_idx(body, '"Hund und Katze"', 'd_german_idx'::regclass) <> (to_tsvector('german', body) @@ phraseto_tsquery('german', 'Hund und Katze'));

-- a phrase with a stop word inside is two apart: neither document without the stop word nor with two words between
CREATE TABLE dg (id int, body text);
INSERT INTO dg VALUES (1, 'der Hund und die Katze'), (2, 'Hund Katze'), (3, 'Hund und Katze'), (4, 'Hund und auch die Katze'), (5, 'Katze und Hund');
CREATE INDEX dg_idx ON dg USING sift (body) WITH (stemmer = 'german');
SELECT id FROM dg WHERE body @@> '"Hund und Katze"' ORDER BY id;
SELECT id FROM dg WHERE body @@> '"der Hund"' ORDER BY id;
SELECT id FROM dg WHERE body @@> '"Hund Katze"' ORDER BY id;
SELECT id FROM dg WHERE body @@> 'und der' ORDER BY id;
SELECT sift.parse_query('Häuser AND "Hund und Katze" OR Wet*', 'dg_idx'::regclass);

-- the metapage keeps the configuration the index was built with: the option naming another one is another analyzer, which verify names
ALTER INDEX dg_idx SET (stemmer = 'french');
SELECT sift.verify('dg_idx');
ALTER INDEX dg_idx SET (stemmer = 'german');
SELECT sift.verify('dg_idx', true);

-- prefixes (word prefixes of the stemmed terms; the wildcard folds, it does not stem), and the indexed prefix terms
CREATE TABLE dp (id int, body text);
INSERT INTO dp SELECT i, gen_doc(words, i) FROM generate_series(1, 300) i, lang_words WHERE cfg = 'german';
CREATE INDEX dp_idx ON dp USING sift (body) WITH (stemmer = 'german', prefix_index = 3);
SELECT count(*) AS wett FROM dp WHERE body @@> 'Wet*';
SELECT count(*) AS wett_pg FROM dp WHERE to_tsvector('german', body) @@ to_tsquery('german', 'wet:*');
SELECT count(*) AS ha FROM dp WHERE body @@> 'ha*';
SELECT count(*) AS ha_pg FROM dp WHERE to_tsvector('german', body) @@ to_tsquery('german', 'ha:*');
SELECT sift.verify('dp_idx', true);

-- accent folding: off by default with a configuration (its own dictionaries decide), honored when set
CREATE TABLE da (id int, body text);
INSERT INTO da VALUES (1, 'une journée été'), (2, 'une journee ete'), (3, 'le Été');
CREATE INDEX da_off ON da USING sift (body) WITH (stemmer = 'french');
CREATE INDEX da_on ON da USING sift (body) WITH (stemmer = 'french', accent_folding = true);
SELECT reloptions FROM pg_class WHERE relname IN ('da_off', 'da_on') ORDER BY relname DESC;
SELECT token, pos FROM sift.tokenize('Une journée été', 'da_off'::regclass);
SELECT token, pos FROM sift.tokenize('Une journée été', 'da_on'::regclass);
SELECT token FROM sift.tokenize('Une journée été', NULL, 'french');
SELECT token FROM sift.tokenize('Une journée été', NULL, 'french', NULL, true);
SELECT sift.verify('da_off', true), sift.verify('da_on', true);
DROP INDEX da_on;
SET enable_seqscan = off;
SELECT id FROM da WHERE body @@> 'ete' ORDER BY id;
SELECT id FROM da WHERE body @@> 'été' ORDER BY id;
RESET enable_seqscan;

-- maintenance: batches, promotion, a forced merge and a REINDEX keep the answers of a fresh build
CREATE TABLE dm (id int PRIMARY KEY, body text);
INSERT INTO dm SELECT i, gen_doc(words, i) FROM generate_series(1, 200) i, lang_words WHERE cfg = 'german';
CREATE INDEX dm_idx ON dm USING sift (body) WITH (stemmer = 'german');
CREATE FUNCTION dm_answers() RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(md5(r), ' ' ORDER BY q) FROM (
		SELECT q, (SELECT string_agg(id::text, ',' ORDER BY id) FROM dm WHERE body @@> q) AS r
		FROM unnest(ARRAY['Haus', 'Häuser', 'Kinder AND Wetter', '"Hund und Katze"', '"Katze die Straße"', 'Wet*', 'schön OR gestern', 'der']) q) x $$;
INSERT INTO dm SELECT i, gen_doc(words, i) FROM generate_series(201, 300) i, lang_words WHERE cfg = 'german';
SELECT sift.promote('dm_idx');
INSERT INTO dm SELECT i, gen_doc(words, i) FROM generate_series(301, 420) i, lang_words WHERE cfg = 'german';
SELECT sift.promote('dm_idx');
INSERT INTO dm SELECT i, gen_doc(words, i) FROM generate_series(421, 500) i, lang_words WHERE cfg = 'german';
DELETE FROM dm WHERE id % 11 = 0;
CREATE TABLE dm_ref (id int PRIMARY KEY, body text);
INSERT INTO dm_ref SELECT * FROM dm;
CREATE FUNCTION dm_ref_answers() RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(md5(r), ' ' ORDER BY q) FROM (
		SELECT q, (SELECT string_agg(id::text, ',' ORDER BY id) FROM dm_ref WHERE body @@> q) AS r
		FROM unnest(ARRAY['Haus', 'Häuser', 'Kinder AND Wetter', '"Hund und Katze"', '"Katze die Straße"', 'Wet*', 'schön OR gestern', 'der']) q) x $$;
CREATE INDEX dm_ref_idx ON dm_ref USING sift (body) WITH (stemmer = 'german');
SELECT dm_answers() = dm_ref_answers() AS batches_same;
SELECT sift.promote('dm_idx');
SELECT sift.merge('dm_idx', true);
SELECT dm_answers() = dm_ref_answers() AS merged_same;
SELECT sift.verify('dm_idx', true);
SELECT count(*) FROM sift.segments('dm_idx') WHERE kind = 'immutable';
VACUUM dm;
SELECT dm_answers() = dm_ref_answers() AS vacuumed_same;
REINDEX INDEX dm_idx;
SELECT dm_answers() = dm_ref_answers() AS reindexed_same;
SELECT sift.verify('dm_idx', true);
SELECT pg_relation_size('dm_idx') = pg_relation_size('dm_ref_idx') AS reindexed_size_same;

-- INCLUDE columns beside a configuration
CREATE TABLE di (id int PRIMARY KEY, body text, n int);
INSERT INTO di SELECT i, gen_doc(words, i), i % 5 FROM generate_series(1, 400) i, lang_words WHERE cfg = 'german';
CREATE INDEX di_idx ON di USING sift (body) INCLUDE (n) WITH (stemmer = 'german');
SET enable_seqscan = off;
SELECT count(*) FROM di WHERE body @@> 'Haus' AND n = 2;
SELECT count(*) FROM di WHERE to_tsvector('german', body) @@ plainto_tsquery('german', 'Haus') AND n = 2;
SELECT count(*) FROM di WHERE body @@> '"Hund und Katze"' AND n >= 3;
SELECT count(*) FROM di WHERE to_tsvector('german', body) @@ phraseto_tsquery('german', 'Hund und Katze') AND n >= 3;
RESET enable_seqscan;
SELECT sift.verify('di_idx', true);

-- a build with workers: the same index answers
CREATE TABLE dw (id int PRIMARY KEY, body text) WITH (parallel_workers = 3);
INSERT INTO dw SELECT i, gen_doc(words, i) FROM generate_series(1, 30000) i, lang_words WHERE cfg = 'russian';
SET max_parallel_maintenance_workers = 0;
CREATE INDEX dw_serial ON dw USING sift (body) WITH (stemmer = 'russian');
CREATE TABLE dw_ans AS SELECT q, (SELECT md5(string_agg(id::text, ',' ORDER BY id)) FROM dw WHERE body @@> q) r
FROM unnest(ARRAY['дом', 'дети AND погода', '"кошка и собака"', 'крас*', 'house OR сигнала']) q;
DROP INDEX dw_serial;
SET max_parallel_maintenance_workers = 3;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '8MB';
CREATE INDEX dw_idx ON dw USING sift (body) WITH (stemmer = 'russian');
SELECT count(*) AS differ FROM dw_ans a WHERE r IS DISTINCT FROM (SELECT md5(string_agg(id::text, ',' ORDER BY id)) FROM dw WHERE body @@> a.q);
SELECT sift.verify('dw_idx', true);

DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
DROP EXTENSION pg_sift;
