-- Column codec (src/col/sift_colcodec*.c): the chooser, round trips, filters on the encoded form, random access, the policy option and the
-- validation of damaged chunks. Sizes are printed only for policies without LZ4/zstd (their bytes depend on the library version).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TEMP TABLE shapes AS
SELECT name, kind, (SELECT array_agg(v ORDER BY g) FROM generate_series(1, 2000) g, LATERAL (SELECT
	CASE name
		WHEN 'constant' THEN 42::int8
		WHEN 'ramp' THEN 1000 + g * 3::int8
		WHEN 'narrow' THEN (g * 7919) % 1000::int8
		WHEN 'runs' THEN (g / 100) * 1000003 + 5000000000::int8
		WHEN 'lowcard' THEN ((g * 7919) % 5) * 1000000007::int8
		WHEN 'outliers' THEN CASE WHEN g % 400 = 0 THEN g * 4611686018427::int8 ELSE (g % 100)::int8 END
		WHEN 'extremes' THEN CASE g % 3 WHEN 0 THEN -9223372036854775807 - 1 WHEN 1 THEN 9223372036854775807 ELSE 0 END
		WHEN 'seconds' THEN 1700000000000000 + ((g * 7919) % 86400)::int8 * 1000000
		WHEN 'prices' THEN sift._test_cc_key(((g * 7919) % 100000) / 100.0::float8)
		WHEN 'mixed floats' THEN sift._test_cc_key(CASE WHEN g % 50 = 0 THEN 'NaN'::float8 WHEN g % 51 = 0 THEN '-Infinity'
												 ELSE ((g * 7919) % 100000) / 100.0::float8 END)
	END AS v) s) AS keys
FROM (VALUES ('constant', 0), ('ramp', 0), ('narrow', 0), ('runs', 0), ('lowcard', 0), ('outliers', 0), ('extremes', 0),
			 ('seconds', 0), ('prices', 2), ('mixed floats', 2)) v(name, kind);

-- the written encoding (tag 1 constant, 2 bit-packed, 3 plain bytes, 4 delta, 5 runs, 6 dictionary, 7 decimal floats, 9 common divisor)
SELECT name, (i).tag, (i).n, (i).bytes
FROM shapes, LATERAL sift._test_cc_info(sift._test_cc_encode(keys, kind, 'for+delta+rle+dict+alp+scale')) i ORDER BY name;
-- a fixed encoding only
SELECT name, (i).tag, (i).bytes
FROM shapes, LATERAL sift._test_cc_info(sift._test_cc_encode(keys, kind, 'for')) i WHERE name IN ('narrow', 'ramp') ORDER BY name;
SELECT name, (i).tag, (i).bytes
FROM shapes, LATERAL sift._test_cc_info(sift._test_cc_encode(keys, kind, 'none')) i ORDER BY name;

-- every policy decodes to the keys, and a column list picks the policy of its column
CREATE TEMP TABLE policies (p text, lib text);
INSERT INTO policies VALUES ('auto', NULL), ('none', NULL), ('lz4', 'lz4'), ('lz4:9', 'lz4'), ('zstd', 'zstd'), ('zstd:19', 'zstd'), ('for', NULL),
	('delta', NULL), ('rle', NULL), ('dict', NULL), ('alp', NULL), ('for+lz4', 'lz4'), ('rle+dict', NULL), ('dict+zstd', 'zstd'), ('scale', NULL), ('scale+rle+dict+zstd', 'zstd'),
	('for+delta+rle+dict+alp+zstd:3', 'zstd'), ('lz4; v=none', NULL), ('none; w=zstd; v=rle+dict', NULL);
-- a build without a library (SIFT_NO_LZ4=1, SIFT_NO_ZSTD=1) skips the policies that name it
DELETE FROM policies WHERE lib IS NOT NULL AND NOT sift._test_cc_has(lib);
SELECT count(*) > 80 AS enough_cases, count(*) FILTER (WHERE sift._test_cc_decode(sift._test_cc_encode(keys, kind, p)) <> keys) AS wrong
FROM shapes, policies;

-- row access and filters (on the encoded form) agree with SQL
CREATE TEMP TABLE ranges (lo int8, hi int8);
INSERT INTO ranges VALUES (-9223372036854775807 - 1, 9223372036854775807), (5, 4), (0, 0), (42, 42), (100, 500), (-1, 1), (1500, 4500),
	(5000000000, 5000300000), (0, 2000000000), (9223372036854775807, 9223372036854775807), (-9223372036854775807 - 1, -9223372036854775807 - 1);
SELECT count(*) > 300 AS enough_cases, count(*) FILTER (WHERE f.rows IS DISTINCT FROM want.rows) AS wrong_rows,
	count(*) FILTER (WHERE (want.sum - f.sum::numeric) % 18446744073709551616 <> 0) AS wrong_sums
FROM (SELECT keys, sift._test_cc_encode(keys, kind, p) AS b FROM shapes, policies WHERE p IN ('auto', 'none', 'zstd', 'rle+dict', 'for')) e
	CROSS JOIN ranges r
	CROSS JOIN LATERAL sift._test_cc_filter(e.b, r.lo, r.hi) f
	CROSS JOIN LATERAL (SELECT coalesce(array_agg((i - 1)::int4 ORDER BY i), '{}') AS rows, coalesce(sum(k::numeric), 0) AS sum
						FROM unnest(e.keys) WITH ORDINALITY u(k, i) WHERE k BETWEEN r.lo AND r.hi) want;
SELECT count(*) > 300 AS enough_gets, count(*) FILTER (WHERE sift._test_cc_get(b, r) <> keys[r + 1]) AS wrong
FROM (SELECT keys, sift._test_cc_encode(keys, kind, p) AS b FROM shapes, policies WHERE p IN ('auto', 'for+delta+rle+dict+alp')) e,
	generate_series(0, 1999, 37) r;

-- NULL slots take the previous value, a leading one the first
SELECT sift._test_cc_decode(sift._test_cc_encode(ARRAY[NULL, NULL, 7, 8, NULL, 9, NULL]::int8[], 0, 'auto'));
SELECT sift._test_cc_decode(sift._test_cc_encode(ARRAY[NULL, NULL]::int8[], 0, 'auto'));
SELECT (sift._test_cc_info(sift._test_cc_encode(ARRAY[NULL, 5, NULL, 9]::int8[], 0, 'auto'))).*;

-- empty and tiny chunks
SELECT n, sift._test_cc_decode(b) AS keys FROM (SELECT 0 AS n, sift._test_cc_encode('{}', 0, 'auto') AS b UNION ALL
	SELECT 1, sift._test_cc_encode('{-5}', 0, 'auto') UNION ALL SELECT 2, sift._test_cc_encode('{1,2}', 0, 'rle')) t ORDER BY n;

-- decimal floats come back as the same floats
SELECT count(*) AS floats, count(*) FILTER (WHERE sift._test_cc_unkey(k) IS DISTINCT FROM sift._test_cc_unkey(o)) AS wrong
FROM (SELECT unnest(sift._test_cc_decode(sift._test_cc_encode(keys, kind, 'alp'))) AS k, unnest(keys) AS o
	  FROM shapes WHERE name = 'mixed floats') t;

-- errors of the policy option
SELECT sift._test_cc_encode('{1,2}', 0, 'gzip');
SELECT sift._test_cc_encode('{1,2}', 0, 'zstd:23');
SELECT sift._test_cc_encode('{1,2}', 0, 'lz4:0');
SELECT sift._test_cc_encode('{1,2}', 0, 'lz4+zstd');
SELECT sift._test_cc_encode('{1,2}', 0, 'none+for');
SELECT sift._test_cc_encode('{1,2}', 0, '') = sift._test_cc_encode('{1,2}', 0, 'auto') AS empty_option_is_auto;
SELECT sift._test_cc_encode('{1,2}', 0, 'auto; auto');

-- damaged chunks: the check word, the header, then the structure with the check word off
CREATE TEMP TABLE good AS SELECT name, sift._test_cc_encode(keys, kind, 'for+delta+rle+dict+alp+scale') AS b FROM shapes;
SELECT sift._test_cc_decode(set_byte(b, 40, get_byte(b, 40) # 255)) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 1, 9)) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 0, 0)) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(substring(b from 1 for 16)) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(substring(b from 1 for length(b) - 8)) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 24, 99), false) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 25, 40), false) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 25, 0), false) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 28, 1), false) FROM good WHERE name = 'runs';
SELECT sift._test_cc_decode(set_byte(b, 2, 7), false) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 4, 0), false) FROM good WHERE name = 'narrow';
SELECT sift._test_cc_decode(set_byte(b, 24, 7), false) FROM good WHERE name = 'narrow';

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
