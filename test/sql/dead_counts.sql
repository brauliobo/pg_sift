-- The dead-posting table that VACUUM stores for a segment (the postings of the deleted docs of each term) feeds the document frequency of every query term:
-- after deletes and updates spread over every superblock, in rounds, the scores are those of the same rows indexed afresh. Terms of every size:
-- some in nearly every doc (posting groups), some in a few (posting lists), one in a single doc.
CREATE EXTENSION pg_sift;
SELECT sift.version() IS NOT NULL AS library_loaded;

CREATE TABLE dc (id int PRIMARY KEY, body text, pad text) WITH (autovacuum_enabled = off, fillfactor = 70);
ALTER TABLE dc ALTER COLUMN pad SET STORAGE PLAIN;
CREATE FUNCTION dc_doc(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT (ARRAY['quick brown fox', 'lazy dog', 'the quick fox jumps', 'brown dog runs fast', 'fox'])[1 + i % 5]
		|| ' w' || i % 7 || ' x' || i % 101 || ' y' || i % 1009 || repeat(' filler', i % 4) || CASE WHEN i % 11 = 0 THEN ' rare' ELSE '' END
		|| CASE WHEN i = 777 THEN ' solitary' ELSE '' END
$$;
INSERT INTO dc SELECT i, dc_doc(i), repeat('x', 1500) FROM generate_series(1, 12000) i;
CREATE INDEX dc_idx ON dc USING sift (body);
SELECT pg_relation_size('dc') / current_setting('block_size')::int / 256 >= 8 AS several_superblocks;

CREATE TABLE qs (q text);
INSERT INTO qs VALUES ('fox'), ('dog'), ('quick fox'), ('filler'), ('w3'), ('x7'), ('y5'), ('rare'), ('fox OR rare'), ('"quick fox"'), ('solitary'), ('fox AND filler'), ('w3 OR x7 OR y5');
-- the rows and scores of every query on a table, in the order of the score
CREATE FUNCTION dc_scores(tbl regclass) RETURNS TABLE (q text, r text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT qs.q, (SELECT string_agg(id || '':'' || round(s::numeric, 5), '','' ORDER BY s DESC, id) FROM
		(SELECT id, sift.score(ctid) AS s FROM %s WHERE body @@> qs.q ORDER BY sift.score(ctid) DESC, id LIMIT 300) t) FROM qs', tbl);
END $$;
-- the scores of dc against those of a copy of its live rows indexed afresh
CREATE FUNCTION dc_same() RETURNS TABLE (queries bigint, same_as_fresh_index bigint) LANGUAGE plpgsql AS $$
BEGIN
	DROP TABLE IF EXISTS dc_ref;
	CREATE TABLE dc_ref AS SELECT * FROM dc;
	CREATE INDEX dc_ref_idx ON dc_ref USING sift (body);
	RETURN QUERY SELECT count(*), count(*) FILTER (WHERE a.r IS NOT DISTINCT FROM b.r) FROM dc_scores('dc') a JOIN dc_scores('dc_ref') b USING (q);
END $$;

-- round 1: a doc in five goes, and every doc whose id ends in 3 changes its text: deletes in every superblock
DELETE FROM dc WHERE id % 5 = 0;
UPDATE dc SET body = body || ' moved' WHERE id % 10 = 3;
VACUUM dc;
SELECT dirty FROM sift.segments('dc_idx') WHERE kind = 'immutable' ORDER BY gen LIMIT 1;
SELECT * FROM dc_same();
-- round 2: a batch promoted next to the segment, and deletes in both
INSERT INTO dc SELECT i, dc_doc(i), repeat('x', 1500) FROM generate_series(12001, 14000) i;
SELECT sift.promote('dc_idx') > 0 AS promoted;
DELETE FROM dc WHERE id % 7 = 2 OR id = 777;
VACUUM dc;
SELECT * FROM dc_same();
-- the dead postings of a segment that is not rebuilt: VACUUM again after one more delete
DELETE FROM dc WHERE id % 13 = 4;
VACUUM dc;
SELECT sift.verify('dc_idx', true);
SELECT * FROM dc_same();

DROP FUNCTION dc_same(), dc_scores(regclass), dc_doc(int);
DROP TABLE dc, dc_ref, qs;
DROP EXTENSION pg_sift;
