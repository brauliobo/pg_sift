-- NULL, empty and token-less documents, documents larger than a page and with more than 64k tokens
-- and 64k distinct terms (built, inserted, promoted and merged), unusual characters:
-- the index agrees with a sequential scan and sift.verify accepts it
CREATE EXTENSION sift;

CREATE FUNCTION ids(tbl regclass, q text) RETURNS text LANGUAGE plpgsql SET enable_seqscan = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY id) FROM %s WHERE body @@> %L', tbl, q) INTO r;
	RETURN coalesce(r, '-');
END $$;
CREATE FUNCTION seq_ids(tbl regclass, q text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY id) FROM %s WHERE body @@> %L', tbl, q) INTO r;
	RETURN coalesce(r, '-');
END $$;
CREATE FUNCTION agree(tbl regclass, q text) RETURNS text LANGUAGE sql
	RETURN ids(tbl, q) || CASE WHEN ids(tbl, q) = seq_ids(tbl, q) THEN '' ELSE ' MISMATCH ' || seq_ids(tbl, q) END;

-- NULLs, empty strings and documents without tokens
CREATE TABLE n (id int, body text);
INSERT INTO n VALUES (1, NULL), (2, ''), (3, '   '), (4, '...'), (5, 'alpha'), (6, 'alpha beta');
CREATE INDEX n_idx ON n USING sift (body);
SELECT agree('n', 'alpha'), agree('n', 'beta'), agree('n', 'alpha NOT beta');
SELECT count(*) FROM n WHERE body @@> 'alpha';
INSERT INTO n VALUES (7, NULL), (8, ''), (9, 'alpha');
UPDATE n SET body = NULL WHERE id = 5;
UPDATE n SET body = 'alpha' WHERE id = 7;
SELECT agree('n', 'alpha');
SELECT sift.promote('n_idx') >= 0 AS promoted;
SELECT agree('n', 'alpha'), agree('n', 'beta');
SELECT sift.verify('n_idx', true);
SELECT count(*) FROM n WHERE body @@> 'alpha';

-- an index over only NULL and empty documents
CREATE TABLE z (id int, body text);
INSERT INTO z VALUES (1, NULL), (2, ''), (3, ' ');
CREATE INDEX z_idx ON z USING sift (body);
SELECT agree('z', 'anything');
SELECT sift.verify('z_idx', true);

-- documents of 1.7 MB (200k tokens, 70k distinct terms), 2 MB (one token) and 1.5 MB (one term)
CREATE TABLE h (id int, body text);
INSERT INTO h SELECT 1, string_agg('tok' || (g % 70000), ' ') FROM generate_series(1, 200000) g;
INSERT INTO h SELECT 2, repeat('a', 2000000);
INSERT INTO h SELECT 3, repeat('word ', 300000);
INSERT INTO h VALUES (4, 'tok5 small');
CREATE INDEX h_idx ON h USING sift (body);
SELECT sift.verify('h_idx', true);
SELECT agree('h', 'tok5'), agree('h', 'tok69999'), agree('h', 'word'), agree('h', 'tok5 AND word');

-- the same documents through aminsert, more than 64k distinct terms in one mutable document
INSERT INTO h SELECT 5, string_agg('tok' || (g % 70000), ' ') FROM generate_series(1, 200000) g;
INSERT INTO h SELECT 6, string_agg('t' || g, ' ') FROM generate_series(1, 66000) g;
SELECT sift.verify('h_idx', true);
SELECT agree('h', 'tok69999'), agree('h', 'tok17990'), agree('h', 't65999');
SELECT sift.promote('h_idx') >= 0 AS promoted;
SELECT sift.verify('h_idx', true);
SELECT agree('h', 'tok69999'), agree('h', 'tok17990'), agree('h', 't65999');
SELECT sift.merge('h_idx', true) >= 0 AS merged;
SELECT sift.verify('h_idx', true);
SELECT agree('h', 'tok69999'), agree('h', 'tok17990'), agree('h', 't65999');
SELECT id, round(sift.score(ctid)::numeric, 4) AS score FROM h WHERE body @@> 'tok5 OR word' ORDER BY sift.score(ctid) DESC, id LIMIT 3;

-- unusual but valid characters: a 100k-character token, noncharacters, astral planes, combining marks, RTL
CREATE TABLE u (id int, body text);
INSERT INTO u VALUES (1, repeat('x', 100000) || ' tail'),
	(2, 'a' || chr(65535) || 'b ' || chr(128512) || ' ' || chr(66376) || ' e' || chr(769) || 'le' || chr(769) || 've'),
	(3, chr(1513) || chr(1500) || chr(1493) || chr(1501) || ' ' || chr(1575) || chr(1604) || chr(1587) || chr(1604) || chr(1575) || chr(1605)),
	(4, chr(65533) || chr(65533) || ' ok'),
	(5, E'tab\tsep\nnew\rline\fform ' || chr(160) || 'nbsp');
CREATE INDEX u_idx ON u USING sift (body);
SELECT sift.verify('u_idx', true);
SELECT agree('u', 'tail'), agree('u', 'ok'), agree('u', chr(128512)), agree('u', 'sep'),
	agree('u', chr(1513) || chr(1500) || chr(1493) || chr(1501)), agree('u', 'nbsp'), agree('u', 'x*');

DROP TABLE n, z, h, u;
DROP FUNCTION agree(regclass, text), ids(regclass, text), seq_ids(regclass, text);
DROP EXTENSION sift;
