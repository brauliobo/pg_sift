-- Ranked top-k scans: exact BM25 scores through the custom scan, ties by ctid,
-- plan shapes and the statements sift.score() may appear in
CREATE EXTENSION pg_sift;

-- the plan text, and the SiftScan node of EXPLAIN (ANALYZE, FORMAT JSON)
CREATE FUNCTION sift_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;
CREATE FUNCTION sift_scan_node(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO j;
	RETURN (SELECT n FROM jsonb_path_query(j, 'strict $.**') n WHERE n ->> 'Custom Plan Provider' = 'SiftScan' LIMIT 1);
END $$;

CREATE TABLE r (id int PRIMARY KEY, body text, grp int);
INSERT INTO r
SELECT i, (ARRAY['quick brown fox', 'lazy dog', 'the quick quick fox jumps', 'brown dog runs fast', 'fox'])[1 + i % 5]
	|| ' w' || i % 7, i % 3
FROM generate_series(1, 300) i;
INSERT INTO r VALUES (301, NULL, 0), (302, '', 0), (303, 'x', 1);
CREATE INDEX r_idx ON r USING sift (body);
ANALYZE r;

-- the top k: descending score, ties by ascending ctid; k beyond the matches returns them all
SELECT id, sift.score(ctid) FROM r WHERE body @@> 'quick fox' ORDER BY sift.score(ctid) DESC LIMIT 5;
SELECT id, sift.score(ctid) FROM r WHERE body @@> 'quick fox' ORDER BY sift.score(ctid) DESC LIMIT 1;
SELECT count(*), min(score), max(score) FROM
	(SELECT sift.score(ctid) AS score FROM r WHERE body @@> 'quick fox' ORDER BY sift.score(ctid) DESC LIMIT 1000) t;
SELECT id, sift.score(ctid) FROM r WHERE body @@> 'fox OR dog^2' ORDER BY sift.score(ctid) DESC, id LIMIT 8;
SELECT id, sift.score(ctid) FROM r WHERE body @@> 'brown NOT dog' ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, sift.score(ctid) FROM r WHERE body @@> '"quick quick"' ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, sift.score(ctid) FROM r WHERE body @@> 'fo* OR /d.g/ OR runz~1' ORDER BY sift.score(ctid) DESC LIMIT 4;
SELECT id, sift.score(ctid) FROM r WHERE body @@> 'nothing' ORDER BY sift.score(ctid) DESC LIMIT 4;

-- the score is the same in the select list and in ORDER BY, with other columns, filters, OFFSET
SELECT id, grp, sift.score(ctid) = sift.score(ctid) AS stable FROM r
WHERE body @@> 'quick' AND grp = 2 ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT id FROM r WHERE body @@> 'quick' AND id % 5 = 0 ORDER BY sift.score(ctid) DESC, id LIMIT 3 OFFSET 2;
SELECT id, round(sift.max_score(ctid)::numeric, 4) FROM r WHERE body @@> 'dog' ORDER BY sift.score(ctid) DESC LIMIT 2;

-- ranking continues past the limit hint when the plan above asks for more rows
SELECT count(*) FROM (SELECT id FROM r WHERE body @@> 'fox' ORDER BY sift.score(ctid) DESC OFFSET 0) t;

-- the unordered scan (no score read) returns the documents a sequential scan finds
CREATE FUNCTION ids_scan(q text) RETURNS int[] LANGUAGE plpgsql
	SET enable_seqscan = off SET enable_bitmapscan = off AS $$
DECLARE
	r int[];
BEGIN
	EXECUTE 'SELECT array_agg(id ORDER BY id) FROM r WHERE body @@> $1' INTO r USING q;
	RETURN r;
END $$;
CREATE FUNCTION ids_seq(q text) RETURNS int[] LANGUAGE sql
	SET sift.enable_scan = off SET enable_bitmapscan = off SET enable_indexscan = off
	RETURN (SELECT array_agg(id ORDER BY id) FROM r WHERE body @@> q);
SELECT q, ids_scan(q) IS NOT DISTINCT FROM ids_seq(q) AS same, coalesce(cardinality(ids_scan(q)), 0) AS n
FROM (VALUES ('quick fox'), ('fox NOT quick'), ('dog OR fox'), ('"quick fox"'), ('"fox quick"'), ('fo*'), ('/d.g/'),
	('runz~1'), ('w3'), ('nothing'), ('quick^2 AND w1')) v(q);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT sift_plan('SELECT id FROM r WHERE body @@> ''w3''');
RESET enable_seqscan;
RESET enable_bitmapscan;

-- plan shapes
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM r WHERE body @@> 'quick fox' ORDER BY sift.score(ctid) DESC LIMIT 5;
EXPLAIN (COSTS OFF) SELECT id FROM r WHERE body @@> 'quick fox' AND grp = 1 ORDER BY sift.score(ctid) DESC LIMIT 5;
SET sift.enable_scan = off;
SELECT id FROM r WHERE body @@> 'quick fox' ORDER BY sift.score(ctid) DESC LIMIT 5;
RESET sift.enable_scan;

-- ties break on the ctid of the visible tuple, not of the index entry (HOT updates move it along the page)
CREATE TABLE h (id int, body text, pad int) WITH (fillfactor = 50, autovacuum_enabled = off);
INSERT INTO h VALUES (1, 'tie', 0), (2, 'tie', 0), (3, 'tie', 0);
CREATE INDEX h_idx ON h USING sift (body);
UPDATE h SET pad = 1 WHERE id IN (1, 2);
SELECT id, ctid FROM h WHERE body @@> 'tie' ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, ctid FROM h WHERE body @@> 'tie' ORDER BY sift.score(ctid) DESC LIMIT 2;
SELECT id, ctid FROM h WHERE body @@> 'tie' ORDER BY sift.score(ctid) DESC LIMIT 1;
DROP TABLE h;

-- parameters and rescans
PREPARE top(text, int) AS SELECT id, sift.score(ctid) FROM r WHERE body @@> $1 ORDER BY sift.score(ctid) DESC LIMIT $2;
EXECUTE top('lazy', 2);
EXECUTE top('fox', 1);
EXECUTE top(NULL, 3);
SELECT t.q, s.id FROM (VALUES ('lazy'), ('runs')) t(q),
	LATERAL (SELECT id FROM r WHERE body @@> t.q ORDER BY sift.score(ctid) DESC LIMIT 2) s ORDER BY 1, 2;

-- the score only exists inside the scan of its relation
SELECT sift.score(ctid) FROM r;
SELECT id FROM r WHERE body @@> 'fox' ORDER BY sift.score(ctid) DESC LIMIT 1 FOR UPDATE;
SELECT id FROM r WHERE id < 5 ORDER BY sift.score(ctid) DESC;

-- the operator follows the index analyzer
CREATE TABLE s (id int, body text);
INSERT INTO s VALUES (1, 'Running dogs'), (2, 'a dog runs'), (3, 'cats run');
CREATE INDEX s_idx ON s USING sift (body) WITH (stemmer = 'english');
ANALYZE s;	-- CREATE INDEX gives the table its size only when the cluster runs an autovacuum, and the plan below follows it
SET enable_seqscan = off;
SELECT id, sift.score(ctid) FROM s WHERE body @@> 'dog running' ORDER BY sift.score(ctid) DESC LIMIT 5;
SELECT id FROM s WHERE body @@> 'runs' ORDER BY id;
SET enable_seqscan = on;
SELECT id FROM s WHERE body @@> 'runs' ORDER BY id;
SELECT regexp_replace(l, '''[0-9]+''::oid', 'OID') AS plan FROM
	(SELECT l FROM unnest(string_to_array(sift_plan('SELECT id FROM s WHERE body @@> ''runs'''), E'\n')) l) t;
SELECT id FROM s WHERE sift.match(body, 'runs') ORDER BY id;
SELECT id FROM s WHERE sift.match_idx(body, 'runs', 's_idx'::regclass) ORDER BY id;
SELECT sift.highlight('Running dogs run', 'dog', '<i>', '</i>', 's_idx');
SELECT sift.highlight('Running dogs run', 'dog', '<i>', '</i>');
DROP TABLE s;

SELECT sift.highlight('The Quick brown fox, quickly', 'quick fox');
SELECT sift.highlight('The Quick brown fox, quickly', 'qui* NOT brown', '[', ']');
SELECT sift.highlight('The "quick fox" jumps', '"quick fox"');
SELECT sift.highlight(NULL, 'x') IS NULL, sift.highlight('x', NULL) IS NULL;

-- EXPLAIN ANALYZE counters: blocks scored and skipped, one index search (bounds per block need a
-- term of at least 65536 documents)
CREATE TABLE big (id int, body text);
INSERT INTO big SELECT i, CASE WHEN i <= 6000 THEN 'beta beta beta beta' ELSE 'beta' END || ' w' || i % 50
	|| repeat(' filler', i % 13) FROM generate_series(1, 70000) i;
CREATE INDEX big_idx ON big USING sift (body);
ANALYZE big;
SELECT j -> 'Index Searches' AS searches, (j ->> 'Blocks Scored')::int > 0 AS scored, (j ->> 'Blocks Skipped')::int > 0 AS skipped,
	(j ->> 'Candidates')::int < 1000 AS few_candidates
FROM (SELECT sift_scan_node('SELECT id FROM big WHERE body @@> ''beta'' ORDER BY sift.score(ctid) DESC LIMIT 3') AS j) t;
SELECT id, sift.score(ctid) FROM big WHERE body @@> 'beta' ORDER BY sift.score(ctid) DESC LIMIT 3;
DROP TABLE big;

-- a score read above a join comes out of the scan, whatever the join method
CREATE TABLE jg AS SELECT g AS grp, 'g' || g AS label FROM generate_series(0, 3) g;
CREATE UNIQUE INDEX jg_grp ON jg (grp);
ANALYZE jg;
CREATE FUNCTION join_diff(q text, q0 text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (((%s) EXCEPT (%s)) UNION ALL ((%s) EXCEPT (%s))) d', q, q0, q0, q) INTO n;
	RETURN n;
END $$;
SELECT sift_plan('SELECT r.id, jg.label, sift.score(r.ctid) FROM r JOIN jg USING (grp) WHERE r.body @@> ''quick fox'' ORDER BY sift.score(r.ctid) DESC LIMIT 5') ~ 'SiftScan' AS scan;
SELECT join_diff('SELECT r.id, sift.score(r.ctid) FROM r JOIN jg USING (grp) WHERE r.body @@> ''quick OR lazy'' ORDER BY sift.score(r.ctid) DESC LIMIT 7',
	'SELECT id, sift.score(ctid) FROM r WHERE grp IN (0, 1, 2, 3) AND body @@> ''quick OR lazy'' ORDER BY sift.score(ctid) DESC LIMIT 7');
SELECT count(*) FROM (SELECT r.id, sift.score(r.ctid) FROM r JOIN jg USING (grp) WHERE r.body @@> 'quick OR lazy' ORDER BY sift.score(r.ctid) DESC LIMIT 7) j;
SET enable_nestloop = off;
SET enable_mergejoin = off;
SELECT join_diff('SELECT r.id, sift.score(r.ctid) FROM r JOIN jg USING (grp) WHERE r.body @@> ''quick OR lazy'' ORDER BY sift.score(r.ctid) DESC LIMIT 7',
	'SELECT id, sift.score(ctid) FROM r WHERE grp IN (0, 1, 2, 3) AND body @@> ''quick OR lazy'' ORDER BY sift.score(ctid) DESC LIMIT 7');
RESET enable_nestloop;
RESET enable_mergejoin;
SET enable_hashjoin = off;
SET enable_nestloop = off;
SELECT join_diff('SELECT r.id, sift.score(r.ctid) FROM r JOIN jg USING (grp) WHERE r.body @@> ''quick OR lazy'' ORDER BY sift.score(r.ctid) DESC LIMIT 7',
	'SELECT id, sift.score(ctid) FROM r WHERE grp IN (0, 1, 2, 3) AND body @@> ''quick OR lazy'' ORDER BY sift.score(ctid) DESC LIMIT 7');
RESET enable_hashjoin;
RESET enable_nestloop;
SELECT r.id, jg.label FROM r JOIN jg USING (grp) WHERE r.body @@> 'quick' ORDER BY sift.score(r.ctid) DESC LIMIT 1;
DROP FUNCTION join_diff(text, text);
DROP TABLE jg;

DROP FUNCTION sift_plan(text), sift_scan_node(text), ids_scan(text), ids_seq(text);
DROP TABLE r;
DROP EXTENSION pg_sift;
