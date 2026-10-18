-- The path spec of the shredder (docs/COL-SHRED.md): canonical forms and errors
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
\set VERBOSITY default

-- the canonical form: one flat item per column, declaration order, "." separators
SELECT sift._test_shred_spec('attributes->locale text, metrics.* float4, items[]{category text, sku text, kind text, qty int4, price float4}, notes[]{type text, author_kind text}');
SELECT sift._test_shred_spec('a.b int4, a.c text, a.d.e bool');
SELECT sift._test_shred_spec('A INT, b SmallInt, c BIGINT, d Real, e DOUBLE PRECISION, f double, g boolean, h Text');
-- groups are sugar for flat paths and merge with them
SELECT sift._test_shred_spec('items[].sku text, items[]{qty int4}, items[].price float4');
SELECT sift._test_shred_spec('a[]{b[]{c int4, d text}, e float8}, f int8');
SELECT sift._test_shred_spec('m[][] float4, t[] text(64), w.*{x int4, y[] text}');
SELECT sift._test_shred_spec('a.*.* int4');
SELECT sift._test_shred_spec('a.* [] text');
-- quoted keys, separators and white space
SELECT sift._test_shred_spec(' "a.b" -> "c d" text ,"say ""hi""" int4');
SELECT sift._test_shred_spec(E'a\n->\tb[ ]\n  text');
SELECT sift._test_shred_spec('naïve int4, ключ text');
SELECT sift._test_shred_spec('$id int8, 2024 text');
-- the fingerprint is stable for the canonical text
SELECT length(sift._test_shred_spec('a int4')) , sift._test_shred_spec('a int4') = sift._test_shred_spec(' a   INT4 ');

-- errors, with the position
SELECT sift._test_shred_spec('');
SELECT sift._test_shred_spec('a');
SELECT sift._test_shred_spec('a nope');
SELECT sift._test_shred_spec('a int4,');
SELECT sift._test_shred_spec('a int4 b text');
SELECT sift._test_shred_spec('a int4 junk');
SELECT sift._test_shred_spec('a.b.');
SELECT sift._test_shred_spec('a - b int4');
SELECT sift._test_shred_spec('a[ int4');
SELECT sift._test_shred_spec('"a int4');
SELECT sift._test_shred_spec('a int4 #');
SELECT sift._test_shred_spec('"" int4');
SELECT sift._test_shred_spec('* int4');
SELECT sift._test_shred_spec('[] int4');
SELECT sift._test_shred_spec('a text(');
SELECT sift._test_shred_spec('a text(x)');
SELECT sift._test_shred_spec('a text(0)');
SELECT sift._test_shred_spec('a text(2000000)');
SELECT sift._test_shred_spec('a text(10');
SELECT sift._test_shred_spec('a int4(10)');
SELECT sift._test_shred_spec('a {b int4}');
SELECT sift._test_shred_spec('a[] {}');
SELECT sift._test_shred_spec('a[] {b int4');
SELECT sift._test_shred_spec('a[] {b int4} c');
SELECT sift._test_shred_spec('a int4, a text');
SELECT sift._test_shred_spec('a int4, a.b text');
SELECT sift._test_shred_spec('a.b text, a int4');
SELECT sift._test_shred_spec('a.* int4, a.b int4');
SELECT sift._test_shred_spec('a.b int4, a.* int4');
SELECT sift._test_shred_spec('a[] int4, a.b int4');
SELECT sift._test_shred_spec('a[] int4, a.* int4');
SELECT sift._test_shred_spec('a[].b int4, a[] int4');
SELECT sift._test_shred_spec('a[]{b int4, b text}');
-- limits
SELECT sift._test_shred_spec('a1.b2.c3.d4.e5.f6.g7.h8.i9.j10.k11.l12.m13.n14.o15.p16 int4');
SELECT sift._test_shred_spec('a1.b2.c3.d4.e5.f6.g7.h8.i9.j10.k11.l12.m13.n14.o15.p16.q17 int4');
SELECT sift._test_shred_spec((SELECT string_agg(format('c%s int4', i), ', ') FROM generate_series(1, 256) i)) IS NOT NULL AS ok256;
SELECT sift._test_shred_spec((SELECT string_agg(format('c%s int4', i), ', ') FROM generate_series(1, 257) i));
SELECT sift._test_shred_spec((SELECT string_agg(format('g%s[] int4', i), ', ') FROM generate_series(1, 63) i)) IS NOT NULL AS ok63;
SELECT sift._test_shred_spec((SELECT string_agg(format('g%s[] int4', i), ', ') FROM generate_series(1, 64) i));
SELECT sift._test_shred_spec(repeat('a int4,', 20000) || 'b int4');
