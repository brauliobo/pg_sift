-- Format major 2. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- >>> format2_build_runs
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- A build that fills its memory many times writes one part of the cold tier for each run and puts them in the manifest a page of parts at a time: every run is there once, at the level of
-- a compaction, the rows add up, the keys are in order and the reads agree with the heap.
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

CREATE TABLE runs (id bigint, grp int, tag text);
INSERT INTO runs SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'tag-' || (i % 500) END FROM generate_series(1, 2000000) i;
SET maintenance_work_mem = '1MB';
CREATE INDEX runs_idx ON runs USING sift_col (id, grp, tag) WITH (key_order = 'id');
RESET maintenance_work_mem;

SELECT count(*) > 36 AS more_runs_than_one_page, count(DISTINCT part_gen) = count(*) AS one_gen_each, min(level) = 2 AND max(level) = 2 AS level_of_a_compaction,
	   sum(nrows) = 2000000 AS rows_add_up, sum(ndead) = 0 AS none_dead
FROM sift._test_f2_parts('runs_idx', 1, NULL);
SELECT regexp_replace(sift._test_f2_check_keys('runs_idx'), 'parts=\d+ granules=\d+', 'parts=N granules=N') AS keys;
SELECT f2_same('SELECT id, grp, tag FROM runs WHERE id BETWEEN 1999000 AND 1999500') AS range_read,
	   f2_same('SELECT grp, count(*), count(tag), count(DISTINCT tag) FROM runs GROUP BY grp') AS aggregate_read,
	   f2_same('SELECT id FROM runs WHERE tag = ''tag-7'' AND id % 1000 = 7') AS filter_read;

DROP TABLE runs;
DROP FUNCTION f2_same(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< format2_build_runs

-- >>> format2_compact_slab_flush
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- A micro-slab of one-row transactions: each row is a compact batch (a 16-byte header and the row as varints), and a slab that reaches 4,096 rows is sealed and flushed into a part.
-- The flush reads every compact batch back, so a batch that is not whole ("ends inside a number") or a sealed chain that takes one more batch shows here as an error or as a wrong count.
\set VERBOSITY terse
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
CREATE TABLE slab_t (id bigint PRIMARY KEY, g int NOT NULL, v numeric(12,2) NOT NULL, s text, ts timestamptz NOT NULL, j jsonb);
INSERT INTO slab_t SELECT g, g % 10, g / 4.0, 's' || g, TIMESTAMPTZ '2024-01-01' + g * interval '1 minute', jsonb_build_object('k', g % 3) FROM generate_series(1, 2000) g;
CREATE INDEX slab_idx ON slab_t USING sift_col (id, g, v, s, ts, j) WITH (track_changes = 'auto');
CREATE SEQUENCE slab_seq START 100000;
DO $$
BEGIN
	FOR i IN 1 .. 4300 LOOP
		INSERT INTO slab_t VALUES (nextval('slab_seq'), i % 10, i / 8.0, CASE WHEN i % 11 = 0 THEN NULL ELSE 'row ' || i END, now(), CASE WHEN i % 5 = 0 THEN NULL ELSE jsonb_build_object('k', i % 3, 'x', i) END);
		COMMIT;
	END LOOP;
END $$;
DO $$
BEGIN
	FOR i IN 1 .. 1500 LOOP
		UPDATE slab_t SET v = v + 1, s = 'upd ' || i WHERE id = i;
		COMMIT;
		IF i % 3 = 0 THEN
			DELETE FROM slab_t WHERE id = 100000 + i;
			COMMIT;
		END IF;
	END LOOP;
END $$;
SELECT count(*) AS check_issues FROM sift.check('slab_idx');
CREATE TEMP TABLE fast AS SELECT count(*) AS n, sum(v) AS sv, sum(g) AS sg, count(s) AS ns, count(j) AS nj, sum(id) AS si FROM slab_t;
SET sift.enable_agg = off;
SET sift.enable_scan = off;
SET sift.enable_col_scan = off;
CREATE TEMP TABLE heap AS SELECT count(*) AS n, sum(v) AS sv, sum(g) AS sg, count(s) AS ns, count(j) AS nj, sum(id) AS si FROM slab_t;
RESET sift.enable_agg;
RESET sift.enable_scan;
RESET sift.enable_col_scan;
SELECT (SELECT row(f.*) FROM fast f) IS NOT DISTINCT FROM (SELECT row(h.*) FROM heap h) AS index_equals_heap;
DROP TABLE slab_t CASCADE;
DROP SEQUENCE slab_seq;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< format2_compact_slab_flush

-- >>> format2_encodings
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- The chunk encodings of format major 2: each column of a granule is written in the encoding that holds it in the fewest bytes (or the one its owner names), read back by the maintenance
-- decoders and by the vector decoder, and a chunk that shrinks by an eighth is wrapped in LZ4 or zstd.
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

-- Integers: a constant, a dense sequence (delta), few distinct values in runs, wide values (frame of reference), small values with nulls
SELECT kind, pattern, sift._test_f2_enc(kind, pattern, 8192, nulls) AS result
FROM (VALUES ('int', 'const', false), ('int', 'seq', false), ('int', 'sorted', false), ('int', 'runs', false), ('int', 'wide', false), ('int', 'small', true), ('int', 'runs', true)) AS c(kind, pattern, nulls);

-- Native fixed-width types and the encodings a column is held to: numeric(p <= 18) as a scaled int64, booleans as bits, the lengths of arrays in blocks of 64 rows
SELECT kind, pattern, sift._test_f2_enc(kind, pattern, 5000, nulls) AS result
FROM (VALUES ('money', 'money', false), ('money', 'money', true), ('bool', 'bool', false), ('bool', 'bool', true), ('list', 'list', false), ('list', 'list', true)) AS c(kind, pattern, nulls);

-- Doubles: decimals are scaled integers (with the others patched in); the others whose top bits take few values are split into a table code and their other bits (all bit patterns
-- exact: -0, NaN payloads, infinities, subnormals), the rest stay as they are
SELECT kind, pattern, sift._test_f2_enc(kind, pattern, 8192, nulls) AS result
FROM (VALUES ('f8', 'const', false), ('f8', 'decimal', false), ('f8', 'decimal', true), ('f8', 'exceptions', false), ('f8', 'exceptions', true), ('f8', 'random', false), ('f8', 'narrow', false), ('f8', 'narrow', true), ('f8', 'special', false), ('f8', 'special', true)) AS c(kind, pattern, nulls);

-- Text: a dictionary when the values repeat, inline otherwise, views when the owner reads them as views; 16-byte uuids, numerics of 38 digits and intervals as they are
SELECT kind, pattern, sift._test_f2_enc(kind, pattern, 8192, nulls) AS result
FROM (VALUES ('str', 'lowcard', false), ('str', 'lowcard', true), ('str', 'unique', false), ('str', 'short', true), ('str', 'mixed', false), ('view', 'mixed', false), ('view', 'unique', true),
			 ('uuid', 'uuid', false), ('uuid', 'uuid', true), ('num128', 'num128', false), ('interval', 'interval', true)) AS c(kind, pattern, nulls);

-- High-cardinality text that no dictionary holds (ids, URLs, free tags) is held in a table of at most 255 symbols of 1 to 8 bytes (FSST), each string apart, when that is not a quarter
-- larger than the codec's list; where the owner keeps compression off, or the text is long on average (64 bytes), or the table would not pay, the list stays
SELECT pattern, nrows, wrap, nulls, sift._test_f2_enc('str', pattern, nrows, nulls, false, wrap) AS result
FROM (VALUES ('tag', 8192, 'lz4', false), ('tag', 8192, 'zstd', true), ('tag', 8192, 'none', false), ('tag', 1000, 'lz4', false), ('tag', 500, 'lz4', false), ('tag', 1, 'lz4', false), ('unique', 8192, 'lz4', false),
			 ('url', 5000, 'zstd', false), ('utf8', 8192, 'lz4', true), ('binary', 8192, 'lz4', false), ('onebyte', 700, 'lz4', false), ('multikb', 300, 'zstd', false), ('empty', 100, 'lz4', false),
			 ('mixed', 8192, 'lz4', false)) AS c(pattern, nrows, wrap, nulls);

-- The symbol table on its own: every string coded and decoded alone and in a run, the table stored and read back, a string cut inside its escape refused; the empty string, a byte, UTF-8, bytes
-- of every value (all escapes) and strings of several kilobytes. Coded sizes against the raw bytes and the LZ4 of the same strings (the ids, URLs and tags of a column are the case)
SELECT pattern, nstrings, sift._test_f2_fsst(pattern, nstrings) AS result
FROM (VALUES ('empty', 1), ('empty', 40), ('onebyte', 1), ('onebyte', 700), ('tag', 1), ('tag', 8192), ('url', 5000), ('utf8', 3000), ('binary', 2000), ('multikb', 200), ('mixed', 4000)) AS c(pattern, nstrings);
SELECT pattern, (m[2]::bigint * 10 < m[1]::bigint * 4) AS under_40_pct_of_raw, (m[2]::bigint * 10 < m[3]::bigint * 7) AS under_70_pct_of_lz4
FROM (VALUES ('tag', 8192), ('url', 5000), ('mixed', 4000)) AS c(pattern, n),
	 LATERAL (SELECT regexp_match(sift._test_f2_fsst(pattern, n, true), 'raw=(\d+) coded=(\d+) lz4=(\d+)') AS m) x;
SELECT pattern, (m[2]::bigint * 10 < m[1]::bigint * 4) AS under_40_pct_of_raw
FROM (VALUES ('utf8', 3000), ('multikb', 200)) AS c(pattern, n),
	 LATERAL (SELECT regexp_match(sift._test_f2_fsst(pattern, n, true), 'raw=(\d+) coded=(\d+)') AS m) x;

-- Rows the column cannot hold (the shredder's overflow and JSON nulls) are nulls with a tag, kept in an exception stream of the chunk
SELECT kind, pattern, sift._test_f2_enc(kind, pattern, 3000, true, true) AS result
FROM (VALUES ('int', 'small'), ('f8', 'decimal'), ('str', 'lowcard'), ('str', 'unique'), ('money', 'money')) AS c(kind, pattern);
SELECT sift._test_f2_enc('int', 'const', 100, true, true);

-- Every row null (with every row tagged: a path whose every value is a JSON null): the chunk has no body; and a single row
SELECT kind, sift._test_f2_enc(kind, 'allnull', 500, true, tags) AS result
FROM (VALUES ('int', false), ('f8', false), ('str', false), ('int', true), ('str', true)) AS c(kind, tags);
SELECT sift._test_f2_enc('int', 'const', 1, false);
SELECT sift._test_f2_enc('str', 'unique', 1, false);

-- Wrapped chunks: only when the compressed bytes are at least an eighth fewer
SELECT kind, pattern, wrap, sift._test_f2_enc(kind, pattern, 8192, false, false, wrap) AS result
FROM (VALUES ('str', 'unique', 'lz4'), ('str', 'unique', 'zstd'), ('str', 'lowcard', 'zstd'), ('int', 'wide', 'lz4'), ('f8', 'random', 'zstd'), ('view', 'mixed', 'lz4'), ('uuid', 'uuid', 'lz4'),
			 ('int', 'runs', 'zstd')) AS c(kind, pattern, wrap);
SELECT sift._test_f2_enc('str', 'unique', 3000, true, true, 'zstd');

-- A row count out of range is refused
SELECT sift._test_f2_enc('int', 'seq', 8193);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< format2_encodings

-- >>> format2_granule
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- Unified granules of format major 2: up to 8192 rows, the _tid as chunk 0, a validity bitmap for the nullable columns, and one encoding for each kind of column chunk.
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');

-- Mode 0: every column valid; 20000 rows are three granules (8192, 8192, 3616)
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 20000, 1000, 1, 0, 0, 0) > 0 AS added;
SELECT g AS gen FROM (SELECT min(part_gen) AS g FROM sift._test_f2_parts('t_idx', 1, NULL)) q \gset
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen, 1000, 1, 0);

-- Mode 1: a column of nothing but nulls and a constant one; mode 2: nulls spread over two columns (a validity bitmap)
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 300, 50000, 900000, 0, 0, 2) > 0 AS added;
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 300, 60000, 910000, 0, 0, 1) > 0 AS added;
SELECT max(part_gen) AS gen2 FROM sift._test_f2_parts('t_idx', 1, NULL) WHERE key_lo = 50000 \gset
SELECT max(part_gen) AS gen3 FROM sift._test_f2_parts('t_idx', 1, NULL) WHERE key_lo = 60000 \gset
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen2, 50000, 900000, 2);
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen3, 60000, 910000, 1);

-- Sort order reversed on insert is the part's order all the same
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 1000, 70000, 2000000, 0, 0, 0, false, true) > 0 AS added;
SELECT max(part_gen) AS gen4 FROM sift._test_f2_parts('t_idx', 1, NULL) WHERE key_lo = 70000 \gset
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen4, 70000, 2000000, 0);
SELECT part_gen > 0 AS ok, level, nrows, key_lo, key_hi, has_xid FROM sift._test_f2_parts('t_idx', 1, NULL) ORDER BY key_lo;
SELECT sift._test_f2_check('t_idx');

-- The time strides of 1 to 4 granules (an even and an odd count) read back exactly
SELECT n, sift._test_f2_tstride('t_idx', n * 8192 - (n % 3) * 100) FROM generate_series(1, 4) n;

DROP TABLE t;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< format2_granule

-- >>> format2_pack_16b
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- Format major 2: a build writes runs of whole granules, as many as its memory holds, the cold codecs take the numbers that come in steps or in a few byte planes, text of a prefix and a number is
-- held as the number (SIFT_PROMO_PREFIX_INT), and the sections a part carries for the reader alone stay under their share; every answer is the heap's, through the parts of the build, the writes after
-- it, and the part a merge joined of them.
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT' END;
END $$;

-- 100000 rows with a number that steps (id, updated_at), a few byte planes (amount, score), identifiers of a prefix and a number (tag, inv, notes.tag) and a key of 300 entities
CREATE TABLE pk (id bigint, entity_id int, event_date date, updated_at timestamptz, tag text, inv text, amount numeric(12,2), score float8, notes jsonb);
INSERT INTO pk SELECT i, 1 + (i * 7919) % 300, DATE '2024-01-01' + (i % 730)::int, TIMESTAMPTZ '2025-01-01' + i * INTERVAL '180 seconds' + ((i * 97) % 1800) * INTERVAL '1 second',
	'tag-' || ((i * 97) % 50000), 'INV-' || lpad(i::text, 8, '0'), ((i * 104729) % 100000) / 100.0, ((i * 31) % 1000) / 10.0, jsonb_build_object('tag', 'n-' || (i % 500))
FROM generate_series(1::bigint, 100000) i;
SET maintenance_work_mem = '6MB';
SET max_parallel_maintenance_workers = 0;
CREATE INDEX pk_idx ON pk USING sift_col (id, entity_id, event_date, updated_at, tag, inv, amount, score, notes) WITH (key_order = 'entity_id, event_date', paths = 'notes.tag text');
RESET maintenance_work_mem;

-- The runs of the build: whole granules; the 100000 rows of narrow cells make two granules to a run under 4MB
SELECT rows, rows % 8192 = 0 AS whole_granules, units FROM sift.col_segments('pk_idx') ORDER BY gen;
SELECT sum(rows) AS rows, count(*) AS parts FROM sift.col_segments('pk_idx');

-- The size of the index under a floor with room over what the codecs make of the numbers and the prefixed text
SELECT pg_relation_size('pk_idx') / 100000.0 < 24 AS under_24_bytes_a_row;
SELECT 'verified' AS index FROM (SELECT sift.col_verify('pk_idx')) v;

CREATE FUNCTION pk_queries(p text) RETURNS TABLE(q text, same text) LANGUAGE sql AS $$
	SELECT q, f2_same(q) FROM unnest(ARRAY[
		'SELECT id, tag FROM ' || p || ' WHERE tag = ''tag-12345''',
		'SELECT id, tag FROM ' || p || ' WHERE tag = ''tag-0''',
		'SELECT id, tag FROM ' || p || ' WHERE tag = ''tag-99999''',
		'SELECT id, tag FROM ' || p || ' WHERE tag = ''tag-012345''',
		'SELECT id, tag FROM ' || p || ' WHERE tag = ''other-12345''',
		'SELECT id, inv FROM ' || p || ' WHERE inv = ''INV-00004242''',
		'SELECT id, inv FROM ' || p || ' WHERE inv = ''INV-4242''',
		'SELECT id, inv FROM ' || p || ' WHERE inv >= ''INV-00099990''',
		'SELECT id, inv FROM ' || p || ' WHERE inv LIKE ''INV-000000%''',
		'SELECT id FROM ' || p || ' WHERE notes->>''tag'' = ''n-312''',
		'SELECT id FROM ' || p || ' WHERE entity_id = 17 AND event_date BETWEEN DATE ''2024-03-01'' AND DATE ''2024-03-31''',
		'SELECT id, updated_at FROM ' || p || ' WHERE updated_at >= TIMESTAMPTZ ''2025-06-01'' AND updated_at < TIMESTAMPTZ ''2025-06-02''',
		'SELECT id, amount, score FROM ' || p || ' WHERE amount BETWEEN 10.00 AND 10.50 AND score = 12.3',
		'SELECT id, amount FROM ' || p || ' WHERE amount = 0.00 OR amount = 999.99',
		'SELECT count(*), sum(id), min(updated_at), max(updated_at) FROM ' || p,
		'SELECT entity_id, count(*), sum(amount) FROM ' || p || ' GROUP BY entity_id HAVING count(*) > 333'
	]) AS t(q) $$;
SELECT * FROM pk_queries('pk');

-- After writes: rows added, changed, and deleted, in the tail beside the parts and in the parts
INSERT INTO pk SELECT i, 1 + (i * 7919) % 300, DATE '2024-01-01' + (i % 730)::int, TIMESTAMPTZ '2025-01-01' + i * INTERVAL '180 seconds', 'tag-' || ((i * 97) % 50000), 'INV-' || lpad(i::text, 8, '0'),
	((i * 104729) % 100000) / 100.0, ((i * 31) % 1000) / 10.0, jsonb_build_object('tag', 'n-' || (i % 500)) FROM generate_series(100001::bigint, 101000) i;
UPDATE pk SET tag = 'tag-' || (id % 7), amount = amount + 1 WHERE id % 1009 = 0;
DELETE FROM pk WHERE id % 211 = 0;
SELECT * FROM pk_queries('pk');
SELECT sift.col_maintain('pk_idx') IS NOT NULL AS maintained;
SELECT * FROM pk_queries('pk');
SELECT 'verified' AS index FROM (SELECT sift.col_verify('pk_idx')) v;

DROP TABLE pk;
DROP FUNCTION f2_same(text), pk_queries(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< format2_pack_16b

-- >>> format2_pack_fsst_auto
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
-- Format major 2: the choice among the float encodings, the dictionary of the strings that a part shares (FSST, and zstd for the cold level), clustering of the rows by a second column only
-- where it pays, parts that are one page or a slot of a page shared by up to 16, a cursor that fetches backwards, and options chosen from the rows (key_order = 'auto', paths left out).
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

-- Doubles: decimals are scaled integers, bounded scientific values (also with outliers, which a sparse patch holds) a table code and the low bits, noise is split too when that is smaller, and
-- bit patterns of every kind (-0, NaN, infinities, subnormals) come back exact
SELECT pattern, wrap, sift._test_f2_enc('f8', pattern, 8192, false, false, wrap) AS result
FROM (VALUES ('decimal'), ('outliers'), ('noisy'), ('wideexp'), ('special'), ('random')) p(pattern), (VALUES ('none'), ('zstd')) w(wrap) ORDER BY pattern, wrap;
SELECT sift._test_f2_enc('f8', 'special', 8192, true, true);

-- Parts that are tiny: each is its header page alone, and the maintenance step writes the tiny ones of level 1 and up together as slots of sealed shared pages (12 parts of 8 rows)
CREATE TABLE pk_a (v int);
CREATE INDEX pk_a_idx ON pk_a USING sift_store_test (v);
CREATE TABLE pk_b (v int);
CREATE INDEX pk_b_idx ON pk_b USING sift_store_test (v);
SELECT sift._test_f2_init('pk_a_idx'), sift._test_f2_init('pk_b_idx');
SELECT sift._test_f2_split_many('pk_a_idx', 1, NULL, 11, 1000), sift._test_f2_split_many('pk_b_idx', 1, NULL, 11, 1000);
SELECT count(sift._test_f2_add_part('pk_a_idx', part_id, key_lo, 8, COALESCE(key_lo, 0) + 10, part_id * 100, 0, 1)) AS parts_a FROM sift._test_f2_partitions('pk_a_idx');
SELECT count(sift._test_f2_add_part('pk_b_idx', part_id, key_lo, 8, COALESCE(key_lo, 0) + 10, part_id * 100, 0, 1)) AS parts_b FROM sift._test_f2_partitions('pk_b_idx');
CREATE FUNCTION pk_info(idx regclass) RETURNS TABLE(part_id int8, key_lo int8, part_gen int8, header_blk int8, packed bool, slot int4, inline bool) LANGUAGE sql AS $$
	SELECT p.part_id, p.key_lo, i.part_gen, i.header_blk, i.packed, i.slot, i.inline
	FROM sift._test_f2_partitions(idx) p, LATERAL sift._test_f2_pack_info(idx, p.part_id, p.key_lo) i $$;
SELECT count(*) AS parts, count(DISTINCT header_blk) AS pages, bool_and(inline) AS inline, bool_or(packed) AS packed FROM pk_info('pk_a_idx');
SELECT sift._test_f2_pack_tails('pk_a_idx') AS packed_parts, sift._test_f2_pack_tails('pk_b_idx') AS packed_parts_b;
SELECT count(*) AS parts, count(DISTINCT header_blk) AS pages, bool_and(packed) AS packed, count(DISTINCT (header_blk, slot)) AS slots FROM pk_info('pk_a_idx');
SELECT sift._test_f2_pack_tails('pk_a_idx') AS packed_again;
SELECT sift._test_f2_check('pk_a_idx') ~ '^parts=12 ' AS consistent;

-- Every row of every packed part reads back as it was written
SELECT count(*) AS parts, count(*) FILTER (WHERE d ~ 'granules=1 rows=8 bad=0 ') AS exact
FROM (SELECT sift._test_f2_dump('pk_a_idx', part_id, key_lo, part_gen, COALESCE(key_lo, 0) + 10, part_id * 100, 0) AS d FROM pk_info('pk_a_idx')) q;

-- The compaction of a part of a shared page leaves the others readable and the page where it is; the compaction of the last one of the page releases it in the same change of the manifest.
-- Both indexes are the same; the second compacts every part of the first page, the first all but one: the page of the second is free again, which leaves the two with the same free blocks.
SELECT header_blk AS first_page FROM pk_info('pk_a_idx') ORDER BY part_id LIMIT 1 \gset
CREATE TABLE pk_x AS SELECT idx, part_id, key_lo, part_gen, row_number() OVER (PARTITION BY idx ORDER BY part_id) AS n, count(*) OVER (PARTITION BY idx) AS total
FROM (SELECT 'pk_a_idx'::regclass AS idx, * FROM pk_info('pk_a_idx') UNION ALL SELECT 'pk_b_idx'::regclass, * FROM pk_info('pk_b_idx')) q WHERE header_blk = :first_page;
SELECT idx, count(*) AS parts_on_the_page FROM pk_x GROUP BY idx ORDER BY idx;
SELECT format('SELECT done FROM sift._test_f2_compact(%L, %s, %s, ARRAY[%s]::int8[])', idx, part_id, coalesce(key_lo::text, 'NULL'), part_gen)
FROM pk_x WHERE n < total OR idx = 'pk_b_idx'::regclass ORDER BY idx, n \gexec
SELECT sift._test_f2_check('pk_a_idx') ~ '^parts=12 ' AS a_consistent, sift._test_f2_check('pk_b_idx') ~ '^parts=12 ' AS b_consistent;
SELECT count(*) FILTER (WHERE packed) AS still_packed FROM pk_info('pk_a_idx');
SELECT sift._test_f2_dump('pk_a_idx', part_id, key_lo, part_gen, COALESCE(key_lo, 0) + 10, part_id * 100, 0) ~ 'granules=1 rows=8 bad=0 ' AS last_part_exact
FROM pk_info('pk_a_idx') WHERE header_blk = :first_page;
SELECT sift._test_f2_reclaim('pk_a_idx') >= 0 AS reclaimed_a, sift._test_f2_reclaim('pk_b_idx') >= 0 AS reclaimed_b;
SELECT sift._test_f2_reclaim('pk_a_idx') >= 0 AS reclaimed_a, sift._test_f2_reclaim('pk_b_idx') >= 0 AS reclaimed_b;
SELECT sift._test_f2_compact('pk_a_idx', part_id, key_lo, ARRAY[part_gen]::int8[]) IS NOT NULL AS last_compacted FROM pk_x WHERE idx = 'pk_a_idx'::regclass AND n = total;
SELECT sift._test_f2_reclaim('pk_a_idx') >= 0 AS reclaimed_a;
SELECT sift._test_f2_reclaim('pk_a_idx') >= 0 AS reclaimed_a;
SELECT (regexp_match(sift._test_f2_fsm('pk_a_idx'), 'meta_free=(\d+)/'))[1]::int - (regexp_match(sift._test_f2_fsm('pk_b_idx'), 'meta_free=(\d+)/'))[1]::int AS free_blocks_apart;
SELECT sift._test_f2_pack_tails('pk_a_idx') >= 0 AS repack_runs, sift._test_f2_check('pk_a_idx') ~ '^parts=12 ' AS consistent;

-- Strings: a table of symbols (FSST) for a whole part, not one for each granule, and for the cold level a zstd dictionary trained on the strings of the part. The answers are those of the heap.
CREATE TABLE sc (id bigint, k text, u text);
INSERT INTO sc SELECT i, substr(md5(i::text), 1, 12), 'https://example.com/' || substr(md5((i * 7)::text), 1, 10) || '/item' || (i % 5000) FROM generate_series(1, 60000) i;
CREATE INDEX sc_idx ON sc USING sift_col (id, k, u) WITH (key_order = 'id');
SELECT sift._test_f2_check_keys('sc_idx');
SELECT sift._test_f2_encs('sc_idx', 1, NULL) ~ 'STR_FSST' AS fsst;
SELECT category, bytes > 0 AS has_bytes FROM sift.index_size_breakdown('sc_idx') WHERE category = 'dicts';
SELECT f2_same('SELECT id FROM sc WHERE k = substr(md5(''777''), 1, 12)');
SELECT f2_same('SELECT id FROM sc WHERE k IN (substr(md5(''777''), 1, 12), substr(md5(''888''), 1, 12), ''nomatch'')');
SELECT f2_same('SELECT id FROM sc WHERE k LIKE ''a1%''');
SELECT f2_same('SELECT id FROM sc WHERE u LIKE ''https://example.com/ab%''');
SELECT f2_same('SELECT count(*), sum(id) FROM sc WHERE u LIKE ''%/item4999''');

CREATE TABLE lg (id bigint, doc text);
INSERT INTO lg SELECT i, (SELECT string_agg(substr(md5('w' || ((i * 31 + j * 17 + (i * j) % 13 + (i * j * j) % 7) % 3000)), 1, 8), ' ') FROM generate_series(1, 30) j) FROM generate_series(1, 24000) i;
CREATE INDEX lg_idx ON lg USING sift_col (id, doc) WITH (key_order = 'id');
SELECT bytes AS dict_bytes_before FROM sift.index_size_breakdown('lg_idx') WHERE category = 'dicts' \gset
DELETE FROM lg WHERE id % 3 = 0;
VACUUM lg;
SELECT :dict_bytes_before AS dict_bytes_at_build, bytes > 0 AS dict_when_cold FROM sift.index_size_breakdown('lg_idx') WHERE category = 'dicts';
SELECT level, nrows FROM sift._test_f2_parts('lg_idx', 1, NULL);
SELECT sift._test_f2_check_keys('lg_idx');
SELECT f2_same('SELECT id, md5(doc) FROM lg WHERE id IN (7, 12345, 23999)');
SELECT f2_same('SELECT count(*), sum(length(doc)) FROM lg WHERE doc LIKE ''%purchase%''');
-- An error while the dictionary of a part is open leaves nothing behind: the next reads of the part are whole
SELECT sum(length(doc)::int / (id::int - 20000)) FROM lg WHERE id > 19000 AND doc IS NOT NULL;
SELECT f2_same('SELECT id, md5(doc) FROM lg WHERE id IN (7, 12345, 23999)');

-- Clustering of the rows by a second column only where it pays at least half a byte a row once the addresses are counted in: rows in the order of their addresses are not reordered
CREATE TABLE cl (id bigint, g int, c int, d int);
INSERT INTO cl SELECT i, i % 4, (i * 7919) % 3, i % 2 FROM generate_series(1, 40000) i;
CREATE INDEX cl_idx ON cl USING sift_col (id, g, c, d) WITH (key_order = 'g');
SELECT sift._test_f2_check('cl_idx') ~ '^parts=[0-9]+ ' AS consistent, sift._test_f2_check_keys('cl_idx') AS keys;
SELECT f2_same('SELECT id FROM cl WHERE g = 2 AND c = 1 AND d = 0');
SELECT f2_same('SELECT g, c, count(*), sum(id) FROM cl GROUP BY g, c');

-- A cursor that fetches backwards returns what the heap does
CREATE TABLE cu (id int, g int, v int);
INSERT INTO cu SELECT i, i % 10, i % 100 FROM generate_series(1, 3000) i;
CREATE INDEX cu_idx ON cu USING sift_col (id, g, v) WITH (key_order = 'g');
CREATE FUNCTION cursor_walk(use_index bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	c SCROLL CURSOR FOR SELECT id FROM cu WHERE g = 3 AND v < 50 ORDER BY id;
	r text := '';
	x int;
BEGIN
	IF use_index THEN SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on;
	ELSE SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; END IF;
	OPEN c;
	FOR i IN 1..4 LOOP FETCH RELATIVE 7 FROM c INTO x; r := r || coalesce(x::text, '-') || ','; END LOOP;
	FOR i IN 1..3 LOOP FETCH RELATIVE -2 FROM c INTO x; r := r || coalesce(x::text, '-') || ','; END LOOP;
	FETCH ABSOLUTE 1 FROM c INTO x; r := r || coalesce(x::text, '-') || ',';
	FETCH LAST FROM c INTO x; r := r || coalesce(x::text, '-') || ',';
	FETCH PRIOR FROM c INTO x; r := r || coalesce(x::text, '-') || ',';
	FETCH PRIOR FROM c INTO x; r := r || coalesce(x::text, '-');
	CLOSE c;
	RETURN r;
END $$;
SELECT cursor_walk(true) = cursor_walk(false) AS cursor_same, cursor_walk(true) AS walk;

-- key_order = 'auto' and paths left out: a table with rows gets both from a sample of them, written to the options so that a rebuild keeps them
CREATE TABLE ev (id bigint, entity_id int, event_date date, data jsonb);
INSERT INTO ev SELECT i, i % 20, date '2026-01-01' + (i % 90), jsonb_build_object('a', i % 7, 'b', 'k' || (i % 11)) FROM generate_series(1, 5000) i;
CREATE INDEX ev_idx ON ev USING sift_col (id, entity_id, event_date, data) WITH (key_order = 'auto');
SELECT reloptions FROM pg_class WHERE relname = 'ev_idx';
SELECT sift._test_f2_state_flags('ev_idx') AS state_flags, sift._test_f2_check('ev_idx') ~ '^parts=[0-9]+ ' AS consistent;
SELECT f2_same('SELECT id FROM ev WHERE data->>''a'' = ''3'' AND entity_id = 4');
SELECT f2_same('SELECT id FROM ev WHERE entity_id = 7 AND event_date >= date ''2026-02-01''');

-- The same on a table with no rows: the index is made, unkeyed and with no leaf for the documents (state flags 6: unkeyed and pending), and every answer is exact: a qual on the documents is the heap's.
CREATE TABLE ev0 (id bigint, entity_id int, event_date date, data jsonb);
CREATE INDEX ev0_idx ON ev0 USING sift_col (id, entity_id, event_date, data) WITH (key_order = 'auto');
SELECT reloptions FROM pg_class WHERE relname = 'ev0_idx';
SELECT sift._test_f2_state_flags('ev0_idx') AS state_flags;
SELECT count(*) AS rows_on_empty FROM ev0 WHERE data->>'a' = '3';
INSERT INTO ev0 SELECT i, i % 20, date '2026-01-01' + (i % 90), jsonb_build_object('a', i % 7, 'b', 'k' || (i % 11)) FROM generate_series(1, 3000) i;
SELECT f2_same('SELECT id FROM ev0 WHERE data->>''a'' = ''3'' AND entity_id = 4');
SELECT f2_same('SELECT id FROM ev0 WHERE entity_id = 4');
-- A REINDEX chooses from the rows it reads, and the flag goes
REINDEX INDEX ev0_idx;
SELECT reloptions FROM pg_class WHERE relname = 'ev0_idx';
SELECT sift._test_f2_state_flags('ev0_idx') AS state_flags;
SELECT f2_same('SELECT id FROM ev0 WHERE data->>''a'' = ''3'' AND entity_id = 4');

-- What the user wrote is never chosen again: key_order = 'off' stays off and the paths of the user stay
CREATE TABLE ev1 (id bigint, entity_id int, event_date date, data jsonb);
INSERT INTO ev1 SELECT i, i % 20, date '2026-01-01' + (i % 90), jsonb_build_object('a', i % 7, 'b', 'k' || (i % 11)) FROM generate_series(1, 2000) i;
CREATE INDEX ev1_idx ON ev1 USING sift_col (id, entity_id, event_date, data) WITH (key_order = 'off', paths = 'data.a int4');
SELECT reloptions FROM pg_class WHERE relname = 'ev1_idx';
REINDEX INDEX ev1_idx;
SELECT reloptions FROM pg_class WHERE relname = 'ev1_idx';
SELECT sift._test_f2_state_flags('ev1_idx') AS state_flags;
SELECT f2_same('SELECT id FROM ev1 WHERE data->>''a'' = ''3''');
-- A table with no entity (a unique id and a timestamp only) takes the time alone, and one with neither stays unkeyed
CREATE TABLE ev2 (id bigint, event_date date, note text);
INSERT INTO ev2 SELECT i, date '2026-01-01' + (i % 90), 'n' || i FROM generate_series(1, 2000) i;
CREATE UNIQUE INDEX ev2_u ON ev2 (id);
CREATE INDEX ev2_idx ON ev2 USING sift_col (id, event_date, note) WITH (key_order = 'auto');
SELECT reloptions FROM pg_class WHERE relname = 'ev2_idx';
CREATE TABLE ev3 (id bigint, note text);
INSERT INTO ev3 SELECT i, 'n' || i FROM generate_series(1, 2000) i;
CREATE INDEX ev3_idx ON ev3 USING sift_col (id, note) WITH (key_order = 'auto');
SELECT reloptions FROM pg_class WHERE relname = 'ev3_idx';
SELECT sift._test_f2_state_flags('ev3_idx') AS state_flags;

DROP TABLE pk_a, pk_b, pk_x, sc, lg, cl, cu, ev, ev0, ev1, ev2, ev3;
-- Partitions that merge carry their parts under the number of the partition they were written for: standalone and packed parts of both open, read and check as before
CREATE TABLE pm (v int);
CREATE INDEX pm_idx ON pm USING sift_store_test (v);
SELECT sift._test_f2_init('pm_idx');
SELECT sift._test_f2_split('pm_idx', 1, NULL, 1000) > 1 AS split;
SELECT sift._test_f2_add_part('pm_idx', 1, NULL, 8, 10, 100, 0, 1) > 0 AS added_low, sift._test_f2_add_part('pm_idx', 2, 1000, 8, 1010, 200, 0, 1) > 0 AS added_high;
SELECT sift._test_f2_pack_tails('pm_idx') AS packed_before_merge;
SELECT sift._test_f2_merge('pm_idx', 1, NULL, 2, 1000);
SELECT count(*) AS parts, count(DISTINCT header_blk) AS pages, bool_and(packed) AS packed FROM sift._test_f2_pack_info('pm_idx', 1, NULL);
SELECT sift._test_f2_check('pm_idx') ~ '^parts=2 ' AS consistent;
SELECT sift._test_f2_dump('pm_idx', 1, NULL, gen, base, tid, 0) ~ 'rows=8 bad=0 ' AS readable
FROM (VALUES ((SELECT min(part_gen) FROM sift._test_f2_parts('pm_idx', 1, NULL)), 10, 100), ((SELECT max(part_gen) FROM sift._test_f2_parts('pm_idx', 1, NULL)), 1010, 200)) v(gen, base, tid) ORDER BY gen;
DROP TABLE pm;
DROP FUNCTION f2_same(text), pk_info(regclass), cursor_walk(bool);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< format2_pack_fsst_auto

-- >>> format2_compressed_skip
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
-- Format major 2 filters the chunks of a granule as they are stored (dictionary codes, runs, packed offsets, deltas, decimal floats, bits, coded strings, with and without nulls), reads
-- the other columns only for the rows that are left, keeps summaries of the values of each granule of a part (skipping indexes) that rule out granules a point, IN or substring
-- predicate cannot match, and rewrites the part that holds the end of the file into the free space below it so that the file shrinks. Every answer is the answer of the heap.
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

-- {exact, recheck, zones, read, sparse, skip probes, skip prunes, parts pruned, compressed filters, granules left without a row}
CREATE FUNCTION scan_stats(OUT probed bool, OUT pruned bool, OUT part_pruned bool, OUT compressed bool, OUT late_skipped bool) LANGUAGE sql AS $$
	SELECT c[6] > 0, c[7] > 0, c[8] > 0, c[9] > 0, c[10] > 0 FROM (SELECT sift._test_f2_scan_counters(true) AS c) s $$;

CREATE FUNCTION run(q text, OUT parity text, OUT probed bool, OUT pruned bool, OUT part_pruned bool, OUT compressed bool, OUT late_skipped bool) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift._test_f2_scan_counters(true);
	parity := f2_same(q);
	SELECT * INTO probed, pruned, part_pruned, compressed, late_skipped FROM scan_stats();
END $$;

-- The shapes of the codecs, on columns with and without nulls: a dictionary, runs, a frame of reference, a delta, a decimal float, a boolean, unique strings
CREATE TABLE cp (id int, s_low text COLLATE "C", r_run int, f_for int, dl int, fl float8, b bool, s_u text COLLATE "C", n_s text COLLATE "C", n_i int, n_u text COLLATE "C");
INSERT INTO cp SELECT i, 'g' || (i % 12), i / 3000, 100000 + i % 1000, i * 3, round((i % 500) / 10.0, 1), i % 3 = 0, 'u-' || lpad(i::text, 6, '0'),
	CASE WHEN i % 7 = 0 THEN NULL ELSE 'v' || (i % 40) END, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 50 END, CASE WHEN i % 9 = 0 THEN NULL ELSE 'w-' || substr(md5(i::text), 1, 10) END
FROM generate_series(1, 30000) i;
CREATE INDEX cp_i ON cp USING sift_col (id, s_low, r_run, f_for, dl, fl, b, s_u, n_s, n_i, n_u) WITH (key_order = 'id');
SELECT sift._test_f2_check('cp_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT n_u AS nu FROM cp WHERE id = 5 \gset

SELECT sift._test_f2_scan_counters(true) IS NOT NULL AS reset;
SELECT n, q, f2_same(q) FROM (VALUES
	(1, 'SELECT id FROM cp WHERE s_low = ''g3'''),
	(2, 'SELECT id FROM cp WHERE s_low <> ''g3'''),
	(3, 'SELECT id FROM cp WHERE s_low IN (''g1'', ''g5'', ''zz'')'),
	(4, 'SELECT id FROM cp WHERE s_low < ''g2'''),
	(5, 'SELECT id FROM cp WHERE s_low >= ''g9'''),
	(6, 'SELECT id FROM cp WHERE s_low = ''absent'''),
	(7, 'SELECT id FROM cp WHERE s_low LIKE ''g1%'''),
	(10, 'SELECT id FROM cp WHERE r_run = 3'),
	(11, 'SELECT id FROM cp WHERE r_run <> 3'),
	(12, 'SELECT id FROM cp WHERE r_run BETWEEN 2 AND 4'),
	(13, 'SELECT id FROM cp WHERE r_run IN (1, 7)'),
	(14, 'SELECT id FROM cp WHERE r_run > 8'),
	(20, 'SELECT id FROM cp WHERE f_for = 100500'),
	(21, 'SELECT id FROM cp WHERE f_for <> 100500'),
	(22, 'SELECT id FROM cp WHERE f_for BETWEEN 100100 AND 100200'),
	(23, 'SELECT id FROM cp WHERE f_for IN (100001, 100999, 5)'),
	(24, 'SELECT id FROM cp WHERE f_for < 100003'),
	(30, 'SELECT id FROM cp WHERE dl = 9000'),
	(31, 'SELECT id FROM cp WHERE dl < 300'),
	(32, 'SELECT id FROM cp WHERE dl >= 89000'),
	(33, 'SELECT id FROM cp WHERE dl BETWEEN 3000 AND 3030'),
	(34, 'SELECT id FROM cp WHERE dl IN (3, 9, 12)'),
	(35, 'SELECT id FROM cp WHERE dl <> 3'),
	(40, 'SELECT id FROM cp WHERE fl = 12.5'),
	(41, 'SELECT id FROM cp WHERE fl <> 12.5'),
	(42, 'SELECT id FROM cp WHERE fl < 0.3'),
	(43, 'SELECT id FROM cp WHERE fl BETWEEN 10 AND 10.5'),
	(44, 'SELECT id FROM cp WHERE fl IN (0.1, 49.9, 3.3)'),
	(50, 'SELECT id FROM cp WHERE b'),
	(51, 'SELECT id FROM cp WHERE NOT b'),
	(52, 'SELECT id FROM cp WHERE b = false'),
	(60, 'SELECT id FROM cp WHERE s_u = ''u-012345'''),
	(61, 'SELECT id FROM cp WHERE s_u <> ''u-012345'''),
	(62, 'SELECT id FROM cp WHERE s_u LIKE ''u-0123%'''),
	(63, 'SELECT id FROM cp WHERE s_u IN (''u-000001'', ''u-029999'', ''nope'')'),
	(64, 'SELECT id FROM cp WHERE s_u LIKE ''%-0123%'''),
	(65, 'SELECT id FROM cp WHERE s_u < ''u-000100'''),
	(66, 'SELECT id FROM cp WHERE s_u LIKE ''%234%'' AND id < 10000'),
	(67, 'SELECT id FROM cp WHERE s_u ILIKE ''U-0123%'''),
	(68, 'SELECT id FROM cp WHERE s_low ILIKE ''%G1%'''),
	(70, 'SELECT id FROM cp WHERE n_s = ''v7'''),
	(71, 'SELECT id FROM cp WHERE n_s <> ''v7'''),
	(72, 'SELECT id FROM cp WHERE n_s IS NULL'),
	(73, 'SELECT id FROM cp WHERE n_s IS NOT NULL AND n_s < ''v2'''),
	(74, 'SELECT id FROM cp WHERE n_s IN (''v1'', ''v39'')'),
	(80, 'SELECT id FROM cp WHERE n_i = 7'),
	(81, 'SELECT id FROM cp WHERE n_i <> 7'),
	(82, 'SELECT id FROM cp WHERE n_i BETWEEN 10 AND 12'),
	(83, 'SELECT id FROM cp WHERE n_i IN (1, 2, 3)'),
	(84, 'SELECT id FROM cp WHERE n_i IS NULL'),
	(85, 'SELECT id FROM cp WHERE n_i > 45'),
	(90, format('SELECT id FROM cp WHERE n_u = %L', :'nu')),
	(91, format('SELECT id FROM cp WHERE n_u <> %L', :'nu')),
	(92, 'SELECT id FROM cp WHERE n_u IS NULL'),
	(93, 'SELECT id FROM cp WHERE n_u LIKE ''w-a%'''),
	(100, 'SELECT id FROM cp WHERE s_low = ''g3'' AND f_for BETWEEN 100100 AND 100400 AND n_i = 3'),
	(101, 'SELECT id FROM cp WHERE r_run = 2 AND dl < 20000 AND fl > 20.0'),
	(102, 'SELECT id FROM cp WHERE n_s = ''v3'' AND n_i IS NULL'),
	(103, 'SELECT id FROM cp WHERE b AND s_low IN (''g0'', ''g3'', ''g6'') AND n_u IS NOT NULL'),
	(104, 'SELECT id FROM cp WHERE s_low = ''absent'' AND f_for = 100500')
) v(n, q) ORDER BY n;
SELECT compressed, late_skipped FROM scan_stats();

-- A predicate no string of the dictionary of the part passes rules out the part, and a part with rows left reads their other columns only
SELECT parity, part_pruned, compressed FROM run('SELECT id FROM cp WHERE s_low = ''absent''');
SELECT parity, late_skipped, compressed FROM run('SELECT id FROM cp WHERE s_low = ''g3'' AND f_for = 100501');

-- High cardinality: a point, an IN list and a substring that the zones cannot rule out are ruled out granule by granule by the summaries kept for each granule (within a hundredth of the
-- bytes of the part: the rows here are wide); the answers are the heap's
CREATE TABLE sk (id int, k_hi int, k_n int, note text COLLATE "C", pad1 bytea, pad2 bytea);
INSERT INTO sk SELECT i, ((i::bigint * 7919) % 1000003)::int, CASE WHEN i % 3 = 0 THEN NULL ELSE ((i::bigint * 104729) % 999983)::int END,
	'ord-' || lpad(i::text, 6, '0') || '-' || (ARRAY['alpha', 'beta', 'gamma', 'delta'])[i % 4 + 1],
	(SELECT decode(string_agg(md5(i::text || 'a' || k), ''), 'hex') FROM generate_series(1, 20) k), (SELECT decode(string_agg(md5(i::text || 'b' || k), ''), 'hex') FROM generate_series(1, 20) k)
FROM generate_series(1, 40000) i;
CREATE INDEX sk_i ON sk USING sift_col (id, k_hi, k_n, note, pad1, pad2) WITH (key_order = 'id');
SELECT sift._test_f2_check('sk_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT k_hi AS hi, k_n AS kn, note AS one FROM sk WHERE id = 31337 \gset

SELECT n, parity, probed, pruned FROM (VALUES
	(1, format('SELECT id FROM sk WHERE k_hi = %s', :hi)),
	(2, format('SELECT id FROM sk WHERE k_hi IN (%s, 5, 99)', :hi)),
	(3, format('SELECT id FROM sk WHERE k_hi = %s AND id > 100', :hi)),
	(4, format('SELECT id FROM sk WHERE k_n = %s', :kn)),
	(5, 'SELECT id FROM sk WHERE k_n = 123456789'),
	(6, format('SELECT id FROM sk WHERE note = %L', :'one')),
	(7, 'SELECT id FROM sk WHERE note IN (''ord-000777-beta'', ''ord-039000-alpha'', ''none'')'),
	(8, 'SELECT id FROM sk WHERE note LIKE ''%-032%'''),
	(9, 'SELECT id FROM sk WHERE note LIKE ''%0390%-alp%'''),
	(10, 'SELECT id FROM sk WHERE note LIKE ''%zzzz%'''),
	(11, 'SELECT id FROM sk WHERE note LIKE ''%\_alpha%'''),
	(12, 'SELECT id FROM sk WHERE k_hi >= 0'),
	(13, 'SELECT id FROM sk WHERE note LIKE ''%ord%''')
) v(n, q), LATERAL run(v.q) ORDER BY n;

-- The end of the file: three batches, the first two deleted; VACUUM compacts what is left, and the part that is then the last in the file is rewritten into the free space below it, and the
-- free run it leaves is cut off the file by the VACUUM itself, so that nothing is left for a later cut
CREATE TABLE tl (id int, p1 bytea, p2 bytea);
CREATE INDEX tl_i ON tl USING sift_col (id, p1, p2) WITH (key_order = 'id');
INSERT INTO tl SELECT i, decode(md5(i::text) || md5(i::text || 'x'), 'hex'), decode(md5(i::text || 'y') || md5(i::text || 'z'), 'hex') FROM generate_series(1, 60000) i;
INSERT INTO tl SELECT i, decode(md5(i::text) || md5(i::text || 'x'), 'hex'), decode(md5(i::text || 'y') || md5(i::text || 'z'), 'hex') FROM generate_series(60001, 120000) i;
INSERT INTO tl SELECT i, decode(md5(i::text) || md5(i::text || 'x'), 'hex'), decode(md5(i::text || 'y') || md5(i::text || 'z'), 'hex') FROM generate_series(120001, 180000) i;
SELECT pg_relation_size('tl_i') / 8192 AS full \gset
DELETE FROM tl WHERE id <= 120000;
-- the extents a VACUUM released are reused once a later transaction id exists, which an idle server has not handed out yet
VACUUM tl;
SELECT pg_current_xact_id() AS xid_after_first \gset
VACUUM tl;
SELECT pg_current_xact_id() AS xid_after_second \gset
SELECT pg_relation_size('tl_i') / 8192 AS pinned \gset
SELECT :pinned <= :full + 16 AS tail_cut_by_vacuum;
SELECT blocks_truncated = 0 AS nothing_left, NOT lock_skipped AS unlocked, file_blocks <= :full + 16 AS back_to_live_size,
	   file_blocks = high_water AS file_ends_at_high_water FROM sift._test_f2_tail_shrink('tl_i');
SELECT sift._test_f2_check('tl_i') ~ '^parts=1 ' AS consistent;
SELECT f2_same('SELECT id FROM tl WHERE id > 100') AS live_rows, f2_same('SELECT id FROM tl WHERE id BETWEEN 120001 AND 120100') AS first_live, f2_same('SELECT id FROM tl WHERE id <= 120000') AS none_dead;

DROP TABLE cp, sk, tl;
DROP FUNCTION run(text), scan_stats(), f2_same(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< format2_compressed_skip

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
