-- Fields: name:"a b" is a phrase of that section only, and a phrase never holds across the boundary of two sections.
CREATE EXTENSION pg_sift;

CREATE FUNCTION sections(name text, city text, rest text) RETURNS text LANGUAGE sql IMMUTABLE AS
	$$ SELECT coalesce(name, '') || chr(29) || coalesce(city, '') || chr(29) || coalesce(rest, '') $$;

CREATE TABLE fp (id int PRIMARY KEY, name text, city text, rest text);
INSERT INTO fp VALUES
	(1, 'red wire', 'blue sky', 'green grass'),
	(2, 'wire red', 'sky blue', 'grass green'),
	(3, 'red', 'wire', 'red wire'),
	(4, 'the red wire', 'blue', 'wire red'),
	(5, 'blue', 'sky blue', 'red wire'),
	(6, 'red wire red wire', '', ''),
	(7, '', 'red wire', ''),
	(8, 'a red', 'wire b', 'wire');
INSERT INTO fp SELECT i, 'p' || i, 'q' || i, 'r' || i FROM generate_series(100, 400) i;
CREATE INDEX fp_idx ON fp USING sift (sections(name, city, rest)) WITH (fields = 'name,city');
CREATE TABLE fq (LIKE fp);
INSERT INTO fq SELECT * FROM fp;
CREATE INDEX fq_idx ON fq USING sift (sections(name, city, rest)) WITH (fields = 'name,city', positions = off);
ANALYZE fp;
ANALYZE fq;

-- the parse of a field phrase, and a field phrase on an index without fields
SELECT sift.parse_query('name:"red wire"', 'fp_idx'::regclass);
SELECT sift.parse_query('city:("red wire" blue)^2', 'fp_idx'::regclass);
SELECT sift.parse_query('name:"red wire"', 'fp_idx'::regclass) = sift.parse_query(sift.parse_query('name:"red wire"', 'fp_idx'::regclass), 'fp_idx'::regclass) AS roundtrip;
CREATE TABLE fn (id int PRIMARY KEY, t text);
INSERT INTO fn VALUES (1, 'name red wire');
CREATE INDEX fn_idx ON fn USING sift (t);
SELECT sift.parse_query('name:"red wire"', 'fn_idx'::regclass);
SELECT sift.parse_query('name:/red/', 'fp_idx'::regclass);

CREATE FUNCTION ids(tbl text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(id::text, '' '' ORDER BY id), ''-'') FROM %I WHERE id < 100 AND sections(name, city, rest) @@> %L', tbl, q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION seqids(tbl text, q text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(id::text, '' '' ORDER BY id), ''-'') FROM %I WHERE id < 100 AND sections(name, city, rest) @@> %L', tbl, q) INTO r;
	RETURN r;
END $$;

-- the phrase of a section: the index (with positions and without), the seq scan agree
SELECT q, ids('fp', q) AS idx, ids('fq', q) AS nopos, seqids('fp', q) AS seq
FROM unnest(ARRAY['name:"red wire"', 'city:"red wire"', 'name:"wire red"', 'city:"blue sky"', 'name:"red wire" city:"blue sky"',
                  'name:"red wire" OR city:"red wire"', 'name:("red wire" "wire red")', 'name:"red the wire"', 'name:"wire"']) q;

-- an unqualified phrase never crosses the boundary of two sections: "red wire" is not held by 'red' | 'wire', nor 'a red' | 'wire b' as "red wire"
SELECT q, ids('fp', q) AS idx, ids('fq', q) AS nopos, seqids('fp', q) AS seq
FROM unnest(ARRAY['"red wire"', '"wire blue"', '"red blue"', '"sky green"', '"blue sky green"', '"red wire" AND NOT name:"red wire"', '"a red wire b"']) q;

-- the same after a write buffer, an update, a promotion, VACUUM and a merge
INSERT INTO fp VALUES (9, 'red', 'wire', 'x'), (10, 'x red wire', 'y', 'z'), (11, 'x', 'red wire', 'z');
INSERT INTO fq SELECT * FROM fp WHERE id IN (9, 10, 11);
SELECT q, ids('fp', q), ids('fq', q), seqids('fp', q) FROM unnest(ARRAY['"red wire"', 'name:"red wire"', 'city:"red wire"']) q;
UPDATE fp SET name = 'red wire' WHERE id = 5;
UPDATE fq SET name = 'red wire' WHERE id = 5;
DELETE FROM fp WHERE id = 1;
DELETE FROM fq WHERE id = 1;
SELECT q, ids('fp', q), ids('fq', q), seqids('fp', q) FROM unnest(ARRAY['"red wire"', 'name:"red wire"', 'city:"red wire"']) q;
VACUUM fp;
VACUUM fq;
SELECT q, ids('fp', q), ids('fq', q), seqids('fp', q) FROM unnest(ARRAY['"red wire"', 'name:"red wire"', 'city:"red wire"']) q;
SELECT sift.verify('fp_idx', true), sift.verify('fq_idx', true);
DROP TABLE fp, fq, fn;
DROP FUNCTION ids(text, text);
DROP FUNCTION seqids(text, text);
DROP FUNCTION sections(text, text, text);
DROP EXTENSION pg_sift;
