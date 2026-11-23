-- The chunk encodings of format major 2: each column of a granule is written in the encoding that holds it in the fewest bytes (or the one its owner names), read back by the maintenance
-- decoders and by the vector decoder, and a chunk that shrinks by an eighth is wrapped in LZ4 or zstd.
CREATE EXTENSION pg_sift_test CASCADE;
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
SELECT pattern, nrows, wrap, nulls, CASE WHEN wrap = 'none' THEN r ELSE regexp_replace(r, 'bytes=[0-9]+', 'bytes=N') END AS result
FROM (VALUES ('tag', 8192, 'lz4', false), ('tag', 8192, 'zstd', true), ('tag', 8192, 'none', false), ('tag', 1000, 'lz4', false), ('tag', 500, 'lz4', false), ('tag', 1, 'lz4', false), ('unique', 8192, 'lz4', false),
			 ('url', 5000, 'zstd', false), ('utf8', 8192, 'lz4', true), ('binary', 8192, 'lz4', false), ('onebyte', 700, 'lz4', false), ('multikb', 300, 'zstd', false), ('empty', 100, 'lz4', false),
			 ('mixed', 8192, 'lz4', false)) AS c(pattern, nrows, wrap, nulls),
	 LATERAL (SELECT sift._test_f2_enc('str', pattern, nrows, nulls, false, wrap) AS r) x;

-- The symbol table on its own: every string coded and decoded alone and in a run, the table stored and read back, a string cut inside its escape refused; the empty string, a byte, UTF-8, bytes
-- of every value (all escapes) and strings of several kilobytes. Coded sizes against the raw bytes and the LZ4 of the same strings (the ids, URLs and tags of a column are the case)
SELECT pattern, nstrings, sift._test_f2_fsst(pattern, nstrings) AS result
FROM (VALUES ('empty', 1), ('empty', 40), ('onebyte', 1), ('onebyte', 700), ('tag', 1), ('tag', 8192), ('url', 5000), ('utf8', 3000), ('binary', 2000), ('multikb', 200), ('mixed', 4000)) AS c(pattern, nstrings);
SELECT pattern, (m[2]::bigint * 10 < m[1]::bigint * 4) AS under_40_pct_of_raw, (m[2]::bigint * 10 < m[3]::bigint * 8) AS under_80_pct_of_lz4
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
SELECT kind, pattern, wrap, regexp_replace(sift._test_f2_enc(kind, pattern, 8192, false, false, wrap), 'bytes=[0-9]+', 'bytes=N') AS result
FROM (VALUES ('str', 'unique', 'lz4'), ('str', 'unique', 'zstd'), ('str', 'lowcard', 'zstd'), ('int', 'wide', 'lz4'), ('f8', 'random', 'zstd'), ('view', 'mixed', 'lz4'), ('uuid', 'uuid', 'lz4'),
			 ('int', 'runs', 'zstd')) AS c(kind, pattern, wrap);
SELECT regexp_replace(sift._test_f2_enc('str', 'unique', 3000, true, true, 'zstd'), 'bytes=[0-9]+', 'bytes=N');

-- A row count out of range is refused
SELECT sift._test_f2_enc('int', 'seq', 8193);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
