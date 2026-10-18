-- The shredder (docs/COL-SHRED.md): absent versus null, empty arrays, mixed types, nesting, numbers, text, limits
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE FUNCTION sh(doc jsonb, spec text, max_elems int DEFAULT 0, max_bytes int DEFAULT 0)
RETURNS TABLE (kind text, path text, parent int, ord int, key text, state text, val text) LANGUAGE sql AS
$$ SELECT s.kind, s.path, s.parent, s.ord, s.key, s.state, s.val FROM sift._test_shred(doc, spec, max_elems, max_bytes) s $$;

-- absent, null, a value, and what is on the way
SELECT * FROM sh('{"a":{"b":1}}', 'a.b int4, a.c int4, d int4, e.f int4');
SELECT * FROM sh('{"a":{"b":null},"d":null,"e":null}', 'a.b int4, a.c int4, d int4, e.f int4');
SELECT * FROM sh('{"a":5,"e":"x"}', 'a.b int4, e.f int4');
SELECT * FROM sh('{"a":[{"b":1}]}', 'a.b int4');
SELECT * FROM sh('[1,2]', 'a int4');
SELECT * FROM sh('5', 'a int4');
SELECT * FROM sh('null', 'a int4');
SELECT * FROM sh('{}', 'a int4, b[] text, c.* int4');

-- arrays: absent, null, empty, one element, not an array
SELECT * FROM sh('{}', 'a[] int4');
SELECT * FROM sh('{"a":null}', 'a[] int4');
SELECT * FROM sh('{"a":[]}', 'a[] int4');
SELECT * FROM sh('{"a":[1]}', 'a[] int4');
SELECT * FROM sh('{"a":7}', 'a[] int4');
SELECT * FROM sh('{"a":{"x":1}}', 'a[] int4');
SELECT * FROM sh('{"a":{"x":1}}', 'a[]{x int4}');

-- arrays of objects: elements keep their position, correlated columns share the element
SELECT * FROM sh('{"items":[{"category":"x","kind":"p","qty":1},{"category":"x","kind":"q"},{"category":"y","kind":"p","qty":3},5,null,[1],{}]}',
                 'items[]{category text, kind text, qty int4}');
-- the same-element rule: an element with category x AND kind p exists in doc 1 only
SELECT d, bool_or(c.state = 'value' AND c.val = 'x' AND k.state = 'value' AND k.val = 'p') AS one_element,
       bool_or(c.val = 'x') AND bool_or(k.val = 'p') AS across_elements
FROM (VALUES (1, '{"items":[{"category":"x","kind":"p"},{"category":"y","kind":"q"}]}'::jsonb),
             (2, '{"items":[{"category":"x","kind":"q"},{"category":"y","kind":"p"}]}')) v(d, doc),
     LATERAL (SELECT * FROM sh(v.doc, 'items[]{category text, kind text}') WHERE path = 'items[].category') c,
     LATERAL (SELECT * FROM sh(v.doc, 'items[]{category text, kind text}') WHERE path = 'items[].kind' AND ord = c.ord) k
GROUP BY d ORDER BY d;

-- nested arrays of objects and arrays of arrays
SELECT * FROM sh('{"o":[{"id":1,"l":[{"v":"a"},{"v":"b"}]},{"id":2},{"id":3,"l":[]},{"id":4,"l":[{"v":"c"}]}]}',
                 'o[]{id int4, l[]{v text}}');
SELECT * FROM sh('{"m":[[1,2],[],[3],null,4,[[5]]]}', 'm[][] int4');
SELECT * FROM sh('{"m":[[{"x":1},{"x":2}],[{"x":3}]]}', 'm[][]{x int4}');

-- wildcard maps: key dictionary, order of jsonb keys, members of other types
SELECT * FROM sh('{"metrics":{"cpu":0.5,"io":2,"net":null,"mem":"high","disk":[1]}}', 'metrics.* float4');
SELECT * FROM sh('{"metrics":{"a":{"n":1,"m":"x"},"b":{"n":2},"c":3}}', 'metrics.*{n int4, m text}');
SELECT * FROM sh('{"metrics":[1]}', 'metrics.* int4');
SELECT * FROM sh('{"metrics":{}}', 'metrics.* int4');
SELECT * FROM sh('{"g":[{"k":{"a":1,"b":2}},{"k":{"b":3}}]}', 'g[]{k.* int4}');

-- duplicate keys collapse in the jsonb: the last one wins; keys are case sensitive and byte exact
SELECT * FROM sh('{"a":1,"a":2,"A":3}', 'a int4, A int4');
SELECT * FROM sh('{"é":1,"é":2}', '"é" int4, "é" int4');

-- integers: exact, range, fractions and exponents
SELECT * FROM sh('{"n":[3,3.0,3.5,1e2,-0,1E+2,12345678901234567890,9223372036854775807,9223372036854775808,-9223372036854775808,-9223372036854775809,"7",true,null,[1],{}]}', 'n[] int8');
SELECT * FROM sh('{"n":[0,32767,32768,-32768,-32769,0.0000,1e0]}', 'n[] int2');
SELECT * FROM sh('{"n":[2147483647,2147483648,-2147483648,-2147483649]}', 'n[] int4');
SELECT * FROM sh('{"n":[100000000000000000000000000000000000000000000000000000000000000000000000000000000000000,1e-5,0.1e1]}', 'n[] int8');

-- floats: the value of the decimal text rounded like float4in / float8in, out of range is an overflow
SELECT * FROM sh('{"n":[0.1,16777217,3.4028235e38,3.5e38,1e-45,1e-46,1e-40,123456789.123456789,-0.0,1e400,1e-400]}', 'n[] float4');
SELECT * FROM sh('{"n":[0.1,9007199254740993,1.7976931348623157e308,1e309,5e-324,1e-330,123456789.123456789,0.30000000000000004]}', 'n[] float8');
SELECT * FROM sh('{"n":[1,2.5,"3",true]}', 'n[] float8');
SELECT * FROM sh('{"n":[10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000]}', 'n[] float8');

-- bool and text
SELECT * FROM sh('{"b":[true,false,null,1,"true",{}]}', 'b[] bool');
SELECT * FROM sh('{"s":["abc","","héllo ✓","日本語",1,true,null,"abcdef","ééé"]}', 's[] text(6)');
SELECT * FROM sh('{"s":["x","tab\there","new\nline","quote\"q","back\\slash"]}', 's[] text');
-- a text column holds 255 bytes unless declared longer
SELECT state, length(val) FROM sh(jsonb_build_object('s', repeat('z', 255)), 's text') WHERE kind = 'col';
SELECT state, length(val) FROM sh(jsonb_build_object('s', repeat('z', 256)), 's text') WHERE kind = 'col';
SELECT state, length(val) FROM sh(jsonb_build_object('s', repeat('z', 256)), 's text(256)') WHERE kind = 'col';

-- limits: lists are cut at max_elems and max_bytes, the overflow is flagged on every column below
SELECT * FROM sh('{"a":[1,2,3,4,5,6]}', 'a[] int4', 4);
SELECT * FROM sh('{"a":[{"x":1,"y":"p"},{"x":2,"y":"q"},{"x":3,"y":"r"}],"b":[1]}', 'a[]{x int4, y text}, b[] int4', 2);
SELECT * FROM sh('{"a":["aaaa","bbbb","cccc"]}', 'a[] text', 0, 9);
SELECT * FROM sh('{"m":{"a":1,"b":2,"c":3}}', 'm.* int4', 2);

-- a spec the document shares nothing with, and nested declared paths that skip siblings
SELECT * FROM sh('{"x":{"y":{"z":[1,2,3]}},"w":1}', 'q int4, x.y.z[] int4');

-- bounded memory: a list is cut at max_elems (1 Mi by default) whatever the size of the array
SELECT * FROM sift._test_shred_summary(jsonb_build_object('a', (SELECT jsonb_agg(i) FROM generate_series(1, 1100000) i)), 'a[] int4');
SELECT kind, path, state, val FROM sh(jsonb_build_object('a', (SELECT jsonb_agg(i) FROM generate_series(1, 20) i)), 'a[] int4', 5) WHERE kind <> 'col';

-- zero-length copies: an empty text and an empty member name as the first of their kind
SELECT * FROM sh('{"s":""}', 's text') WHERE kind = 'col';
SELECT * FROM sh('{"m":{"":1,"a":2}}', 'm.* int4') WHERE kind = 'col';

-- the shredder is kept between documents: nothing of the previous one remains
SELECT id, sift._test_shred_count(doc, 'items[]{category text, qty int4}, tags[] text, m.* int4')
FROM (VALUES (1, '{"items":[{"category":"x","qty":1},{"category":"y","qty":2},{"category":"z"}],"tags":["a","b"],"m":{"a":1,"b":2}}'::jsonb),
			 (2, '{}'), (3, '{"tags":["c"]}'), (4, '{"items":[{"qty":9}]}'), (5, '[1]'), (6, '{"m":{"a":1}}')) v(id, doc) ORDER BY id;
