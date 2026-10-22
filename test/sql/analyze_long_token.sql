-- a word over a term long (255 bytes) is its start, U+FFFF and a hash of all of it: words that share their start stay apart
CREATE EXTENSION pg_sift;

CREATE TABLE lt (id int, body text);
INSERT INTO lt VALUES (1, repeat('a', 280) || 'one'), (2, repeat('a', 280) || 'two'), (3, 'other'),
	(4, repeat('a', 255)), (5, repeat('a', 256)), (6, repeat('ж', 128)), (7, repeat('ж', 127) || 'з'),
	(8, 'before ' || repeat('Ab1', 100) || ' after');
CREATE INDEX lt_idx ON lt USING sift (body);

CREATE FUNCTION lt_ids(q text) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM lt WHERE body @@> q $$;
CREATE FUNCTION lt_ids_seq(q text) RETURNS text LANGUAGE sql SET enable_bitmapscan = off SET sift.enable_scan = off AS $$
	SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM lt WHERE body @@> q $$;

-- tokens: up to 255 bytes as written, longer ones hashed, whole characters only, offsets cover the word
SELECT id, octet_length(f.token) AS bytes, right(f.token, 16) AS tail, f.pos, f.start_offset, f.end_offset
FROM lt JOIN LATERAL sift.tokenize(body, 'lt_idx') f ON true WHERE octet_length(body) > 200 ORDER BY id, f.pos;

-- the index finds the word asked for and no other with the same start; a scan of the heap agrees
SELECT left(q, 1) || '*' || length(q) || right(q, 3) AS word, lt_ids(q) AS ids, lt_ids(q) = lt_ids_seq(q) AS same
FROM (VALUES (repeat('a', 280) || 'one'), (repeat('a', 280) || 'two'), (repeat('a', 280) || 'six'), (repeat('a', 255)),
	(repeat('a', 256)), (repeat('ж', 128)), (repeat('ж', 127) || 'з'), (repeat('AB1', 100)), (repeat('ab1', 100))) v(q);

-- a prefix longer than the start of a hashed word finds it
SELECT length(q) AS prefix_len, lt_ids(q || '*') AS ids, lt_ids(q || '*') = lt_ids_seq(q || '*') AS same
FROM (VALUES (repeat('a', 100)), (repeat('a', 240)), (repeat('a', 280))) v(q);

SELECT sift.verify('lt_idx', true);
INSERT INTO lt VALUES (9, repeat('a', 280) || 'one');
SELECT lt_ids(repeat('a', 280) || 'one'), lt_ids(repeat('a', 280) || 'two');
SELECT sift.verify('lt_idx', true);

DROP TABLE lt;
DROP EXTENSION pg_sift;
