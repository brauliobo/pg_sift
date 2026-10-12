-- hostile and oversized queries: limits are errors, never crashes or unbounded work
CREATE EXTENSION sift;
CREATE TABLE q (id int, body text);
INSERT INTO q SELECT i, 'tok' || i % 700 || ' common word' || i % 50 FROM generate_series(1, 3000) i;
CREATE INDEX q_idx ON q USING sift (body);
ANALYZE q;

CREATE FUNCTION hits(query text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM q WHERE body @@> $1' INTO n USING query;
	RETURN n::text;
END $$;
CREATE FUNCTION hits_seq(query text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM q WHERE body @@> $1' INTO n USING query;
	RETURN n::text;
END $$;
CREATE FUNCTION same(query text) RETURNS text LANGUAGE sql
	RETURN hits(query) || CASE WHEN hits(query) = hits_seq(query) THEN '' ELSE ' MISMATCH ' || hits_seq(query) END;

-- many terms
SELECT same((SELECT string_agg('tok' || g, ' OR ') FROM generate_series(1, 200) g));
SELECT same((SELECT string_agg('tok' || g, ' OR ') FROM generate_series(1, 256) g));
SELECT hits((SELECT string_agg('tok' || g, ' OR ') FROM generate_series(1, 257) g));
SELECT hits((SELECT string_agg('tok' || g, ' OR ') FROM generate_series(1, 200000) g));
-- long queries
SELECT hits(repeat('x', 262144));
SELECT hits(repeat('x', 262145));
SELECT hits(repeat('x', 10000000));
SELECT hits(repeat('word1 ', 40000));
-- nesting
SELECT same(repeat('(', 30) || 'tok5' || repeat(')', 30));
SELECT hits(repeat('(', 100000) || 'tok5' || repeat(')', 100000));
SELECT hits(repeat('NOT ', 100000) || 'tok5');
SELECT hits(repeat('(', 100));
SELECT hits(repeat(')', 100));
-- phrases
SELECT same('"' || (SELECT string_agg('tok' || g, ' ') FROM generate_series(1, 100) g) || '"');
SELECT hits('"' || (SELECT string_agg('tok' || g, ' ') FROM generate_series(1, 1000) g) || '"');
-- expansions
SELECT same('tok12*'), same('tok1*'), same('*k12'), same('/tok1[0-9]/'), same('tok12~1');
SELECT hits('tok*');
SELECT hits('t*');
SELECT hits('*');
SELECT hits(repeat('a', 300) || '*');
SELECT hits('t' || repeat('o', 500) || '~2');
SELECT hits('tok1~9');
SELECT hits('/(((((((((((a*)*)*)*)*)*)*)*)*)*)*)*b/');
SELECT hits('/' || repeat('a?', 5000) || '/');
SELECT hits('/' || repeat('(', 3000) || 'a' || repeat(')', 3000) || '/');
-- boosts, odd operators, odd characters, empty queries
SELECT hits('tok5^0');
SELECT hits('tok5^99999999999999999999');
SELECT hits('^');
SELECT hits('AND');
SELECT hits('tok5 AND');
SELECT hits('OR OR');
SELECT hits('tok5 e' || chr(769) || ' ' || chr(128512));
SELECT hits(chr(1) || 'tok5');
SELECT hits('tok5' || E'\n\tOR\ttok6');
SELECT hits('');
SELECT hits('   ');
SELECT hits('-');
SELECT hits('"');
SELECT hits('"" tok5');

DROP TABLE q;
DROP FUNCTION same(text), hits(text), hits_seq(text);
DROP EXTENSION sift;
