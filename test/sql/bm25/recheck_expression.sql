-- phrase rechecks of the SiftScan / SiftCount executor read the indexed expression of the heap tuple
CREATE EXTENSION pg_sift;

CREATE TABLE r (id int, a text, b text);
INSERT INTO r SELECT i, 'one two ' || i % 3 || ' three', (ARRAY['x', 'y', NULL])[1 + i % 3] FROM generate_series(1, 300) i;
CREATE INDEX r_idx ON r USING sift ((a || ' ' || coalesce(b, '')));
VACUUM ANALYZE r;

SELECT count(*) FROM r WHERE (a || ' ' || coalesce(b, '')) @@> '"two 1 three"';
SELECT count(*) FROM r WHERE (a || ' ' || coalesce(b, '')) @@> '"two 1 three" y';
SELECT count(*) FROM r WHERE (a || ' ' || coalesce(b, '')) @@> '"two 1 three" x';
SELECT id FROM r WHERE (a || ' ' || coalesce(b, '')) @@> '"two 0 three" x' ORDER BY id LIMIT 3;
SET enable_seqscan = off;
SELECT id, sift.score(ctid) > 0 AS scored FROM r WHERE (a || ' ' || coalesce(b, '')) @@> '"two 2 three"'
	ORDER BY sift.score(ctid) DESC, id LIMIT 3;

DROP TABLE r;
DROP EXTENSION pg_sift;
