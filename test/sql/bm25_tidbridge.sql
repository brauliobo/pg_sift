-- A ranked search restricted to a bridge of TIDs (core/tidbridge.h) returns what the same query returns among those TIDs: the bridge only decides which superblocks are
-- read (its own, when they are few), which pages of a superblock are looked at before the docmap, and which offsets of a block are scored; its lossy pages and its
-- empty and point sets give the same hits.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
CREATE TABLE tb (id int PRIMARY KEY, body text);
INSERT INTO tb SELECT i, 'w' || i % 10 || ' x' || i % 7 || repeat(' w3', i % 4) || ' tk' || lpad((i % 700)::text, 3, '0') || ' tk' || lpad((i % 311)::text, 3, '0') ||
	repeat(' padding filler words of the document', 6 + i % 9) FROM generate_series(1, 20000) i;
CREATE INDEX tb_idx ON tb USING sift (body);
-- documents of the write buffer, not yet in a segment
INSERT INTO tb SELECT i, 'w' || i % 10 || ' x' || i % 7 || ' tk' || lpad((i % 700)::text, 3, '0') FROM generate_series(20001, 20300) i;
ANALYZE tb;
RESET client_min_messages;

-- several superblocks, so that a bridge of one of them is far fewer than the leaves' own
SELECT sum(sb_hi - sb_lo + 1) > 4 AS several_superblocks FROM sift.segments('tb_idx'::regclass);

CREATE FUNCTION bt_ref(q text, tids tid[], k int8) RETURNS TABLE (ctid tid, score float4) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT r.ctid, r.score FROM (SELECT t.ctid, sift.score(t.ctid) AS score FROM tb t WHERE body @@> %L ORDER BY sift.score(t.ctid) DESC, t.ctid) r
		WHERE r.ctid = ANY($1) ORDER BY r.score DESC, r.ctid LIMIT $2', q) USING tids, NULLIF(k, 0);
END $$;

-- hits that differ in place, score or tid between the search under the bridge and the reference
CREATE FUNCTION bt_diff(q text, tids tid[], k int8, budget_kb int) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM (SELECT row_number() OVER () AS n, ctid, score FROM bt_ref(q, tids, k)) a
		FULL JOIN (SELECT row_number() OVER () AS n, ctid, score FROM sift._test_bridge_search('tb_idx'::regclass, q, tids, k, budget_kb)) b USING (n)
	WHERE a.ctid IS DISTINCT FROM b.ctid OR a.score IS DISTINCT FROM b.score
$$;

CREATE TABLE bt_sets AS
SELECT name, tids FROM (VALUES
	('point', ARRAY(SELECT ctid FROM tb WHERE id IN (10, 5000, 9000, 15000, 19999, 20100))),
	('one_superblock', ARRAY(SELECT ctid FROM tb WHERE id <= 60)),
	('every_third', ARRAY(SELECT ctid FROM tb WHERE id % 3 = 0)),
	('all', ARRAY(SELECT ctid FROM tb)),
	('none', ARRAY[]::tid[])) s(name, tids);
SELECT name, cardinality(tids) AS tids FROM bt_sets ORDER BY name;

CREATE TABLE bt_q (q text);
INSERT INTO bt_q VALUES ('w1'), ('w3'), ('w1 w2'), ('w1 AND x3'), ('w3 OR w5^2'), ('w1 OR x3 OR w3'), ('tk*'), ('tk* AND w1'), ('tk0* OR w4');
SET sift.max_expansions = 50;

SELECT key AS reset FROM sift._test_read_stats(true) WHERE key = 'inverted_segs';
SELECT s.name, k, sum(bt_diff(q, s.tids, k, 4096)) AS mismatching_exact
FROM bt_sets s, bt_q, unnest(ARRAY[1, 10, 1000, 0]) k GROUP BY s.name, k ORDER BY s.name, k;

-- a budget under the bridge's pages makes them lossy: more offsets are scored and the verification of the caller keeps the same hits
SELECT s.name, sum(bt_diff(q, s.tids, k, 1)) AS mismatching_lossy
FROM bt_sets s, bt_q, unnest(ARRAY[10, 0]) k WHERE s.name IN ('one_superblock', 'every_third', 'point') GROUP BY s.name ORDER BY s.name;
RESET sift.max_expansions;
-- the bridge of a few superblocks drove the units of the segment, and a superblock none of whose pages the bridge holds was not read
SELECT key, value > 0 AS nonzero FROM sift._test_read_stats() WHERE key IN ('inverted_segs', 'bridge_docmap_skips') ORDER BY key;

-- the hits are those of the bridge: none of another TID, and as many as the matches among its TIDs
SELECT count(*) = 0 AS only_the_bridge FROM sift._test_bridge_search('tb_idx'::regclass, 'w1', (SELECT tids FROM bt_sets WHERE name = 'one_superblock'), 0, 4096) h
WHERE NOT EXISTS (SELECT 1 FROM bt_sets s, unnest(s.tids) u WHERE s.name = 'one_superblock' AND u = h.ctid);
SELECT count(*) = (SELECT count(*) FROM tb WHERE body @@> 'w1' AND id <= 60) AS all_of_them
FROM sift._test_bridge_search('tb_idx'::regclass, 'w1', (SELECT tids FROM bt_sets WHERE name = 'one_superblock'), 0, 4096);

DROP FUNCTION bt_diff(text, tid[], int8, int), bt_ref(text, tid[], int8);
DROP TABLE bt_q, bt_sets, tb;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
