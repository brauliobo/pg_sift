-- Word positions: a required phrase is decided by the index, which reads no heap text. Counts and
-- ranked results with their scores equal positions = off (heap recheck) and sift.match, through
-- mutable segments, promotion, merge, deletes and VACUUM.
CREATE EXTENSION pg_sift;

CREATE TABLE po (id int PRIMARY KEY, body text);
CREATE TABLE pf (id int PRIMARY KEY, body text);
-- repeated words, stems, accents, short documents and phrases at both ends; the last document has
-- more tokens than a document length can hold
INSERT INTO po
SELECT i, concat_ws(' ', w[1 + i % 6], w[1 + (i / 6) % 6], w[1 + (i / 3) % 6], w[1 + (i * 7) % 6], w[1 + (i / 5) % 6], 'x' || i % 9)
FROM generate_series(1, 900) i, LATERAL (SELECT ARRAY['red', 'running', 'Café', 'quick', 'fox', 'runs'] w) a;
INSERT INTO po VALUES (901, NULL), (902, ''), (903, 'red'), (904, 'red red red red'), (905, 'red running red running red'),
	(906, 'cafe runs'), (907, 'x1 red'), (908, 'red fox ' || repeat('quick ', 70000) || 'fox red');
INSERT INTO pf SELECT * FROM po;
CREATE INDEX po_idx ON po USING sift (body);
CREATE INDEX pf_idx ON pf USING sift (body) WITH (positions = off);
VACUUM ANALYZE po;
VACUUM ANALYZE pf;

SELECT reloptions FROM pg_class WHERE relname = 'pf_idx';
SELECT sift.verify('po_idx', true), sift.verify('pf_idx', true);

CREATE FUNCTION qs() RETURNS SETOF text LANGUAGE sql AS $$ SELECT * FROM unnest(ARRAY[
	'"red running"', '"running red"', '"red red"', '"red red red"', '"red running red"', '"red running red running"',
	'"quick fox x1"', '"cafe red"', '"CAFÉ   Red"', '"fox red"', '"red fox"', '"quick quick"', '"quick fox"',
	'"runs cafe"', '"run runs"', '"x1 red"', '"red x1"', '"red"', '"nothing here"', '"red nothing"',
	'"red running" AND fox', '"red running" OR "quick fox"', '"red running" NOT fox', 'fox NOT "red running"',
	'"red running" NOT "running cafe"', '"red running"^3 OR fox', '"red running" "fox quick" runs',
	'(fox OR quick) "red running"', 're* "running cafe"', '/ru.*/ OR "fox quick"', 'rad~1 "red running"',
	'"red running" "running cafe"', '"fox red" AND "red fox"']) $$;

CREATE FUNCTION cnt(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE body @@> %L', tbl, q) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION oracle(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE sift.match(body, %L)', tbl, q) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION top(tbl text, q text, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id || ''='' || s::text, '','' ORDER BY s DESC, id) FROM
		(SELECT id, sift.score(ctid) s FROM %I WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT %s) t', tbl, q, k)
	INTO r;
	RETURN r;
END $$;
-- the queries whose counts or ranked results differ between the two indexes and the oracle
CREATE FUNCTION diffs() RETURNS TABLE (q text, positions bigint, plain bigint, oracle bigint) LANGUAGE sql AS $$
	SELECT q, cnt('po', q), cnt('pf', q), oracle('pf', q) FROM qs() q
	WHERE cnt('po', q) <> oracle('pf', q) OR cnt('pf', q) <> oracle('pf', q)
		OR top('po', q, 20) IS DISTINCT FROM top('pf', q, 20) OR top('po', q, 2000) IS DISTINCT FROM top('pf', q, 2000)
$$;
SELECT * FROM diffs();
SELECT count(*) AS queries, sum(cnt('po', q)) AS hits FROM qs() q;
SELECT top('po', '"red running"', 3), top('po', '"red running" OR fox', 3);

-- the same through the bitmap heap scan
SET sift.enable_scan = off;
SET sift.enable_count = off;
SELECT q, cnt('po', q) AS positions, oracle('pf', q) AS oracle FROM qs() q WHERE cnt('po', q) <> oracle('pf', q);
RESET sift.enable_scan;
RESET sift.enable_count;

-- a required or an optional phrase reads no heap page; positions = off and a phrase under NOT recheck
CREATE FUNCTION heap_checks(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON)
		SELECT count(*) FROM %I WHERE body @@> %L', tbl, q) INTO j;
	RETURN (SELECT (n ->> 'Heap Checks')::bigint FROM jsonb_path_query(j, 'strict $.**') n WHERE n ? 'Heap Checks' LIMIT 1);
END $$;
SELECT heap_checks('po', '"red running"') AS required, heap_checks('pf', '"red running"') > 0 AS off_required,
	heap_checks('po', '"red running red"') AS three, heap_checks('po', '"red running" AND fox') AS conj,
	heap_checks('po', '"red running" OR fox') AS optional, heap_checks('po', 'fox NOT "red running"') > 0 AS negated;

-- rare words (one or two documents) keep their positions in the dictionary: phrases of them read no heap either
CREATE TABLE pr (id int PRIMARY KEY, body text);
INSERT INTO pr VALUES (1, 'zork blat quux'), (2, 'blat zork'), (3, 'quux zork blat zork blat'), (4, 'common words only'),
	(5, 'common ' || repeat('longw ', 300) || 'blat'), (6, 'zork common blat');
INSERT INTO pr SELECT 10 + i, 'common filler ' || i FROM generate_series(1, 200) i;
CREATE INDEX pr_idx ON pr USING sift (body);
SELECT sift.verify('pr_idx', true);
SELECT q, cnt('pr', q) AS hits, oracle('pr', q) AS oracle FROM unnest(ARRAY['"zork blat"', '"blat zork"', '"quux zork blat"',
	'"zork blat" OR "common words"', '"longw longw"', '"longw blat"', '"zork blat" NOT quux', '"blat zork blat"']) q;
-- the records over 256 bytes (longw) are not kept inline: that term is decided by the heap
SELECT heap_checks('pr', '"quux zork"') AS rare, heap_checks('pr', '"quux zork blat"') AS rare3,
	heap_checks('pr', '"quux zork" OR "common words"') AS optional, heap_checks('pr', '"longw blat"') > 0 AS long_record;
DROP TABLE pr;

-- documents in mutable segments have no positions: the heap decides them, the rest stays in the index
INSERT INTO po SELECT 1000 + i, 'fox red running ' || 'x' || i % 4 FROM generate_series(1, 40) i;
INSERT INTO pf SELECT * FROM po WHERE id > 1000;
SELECT * FROM diffs();
SELECT heap_checks('po', '"red running"') > 0 AS mutable_rechecked;
SELECT sift.promote('po_idx') IS NOT NULL AS promoted, sift.promote('pf_idx') IS NOT NULL AS promoted_plain;
SELECT * FROM diffs();
SELECT sift.verify('po_idx', true);

-- deletes, updates and VACUUM; the promoted segment has no positions, the first one keeps them
DELETE FROM po WHERE id % 7 = 0;
DELETE FROM pf WHERE id % 7 = 0;
UPDATE po SET body = body || ' red running' WHERE id % 11 = 0;
UPDATE pf SET body = body || ' red running' WHERE id % 11 = 0;
SELECT * FROM diffs();
VACUUM po;
VACUUM pf;
SELECT * FROM diffs();
SELECT sift.verify('po_idx', true);

-- merging segments with and without positions drops the positions of the merged segment (pf follows po:
-- scores follow the segments' statistics)
SELECT sift.merge('po_idx', true) IS NOT NULL AS merged, sift.merge('pf_idx', true) IS NOT NULL AS merged_plain;
SELECT * FROM diffs();
SELECT sift.verify('po_idx', true);
INSERT INTO po SELECT 3000 + i, 'running red fox ' || 'x' || i % 3 FROM generate_series(1, 40) i;
INSERT INTO pf SELECT * FROM po WHERE id > 3000;
SELECT * FROM diffs();

-- an index of an existing table, every combination of options: pairs with and without positions
CREATE INDEX po_pairs ON po USING sift (body) WITH (phrase_index = pairs);
CREATE INDEX po_pairs_off ON po USING sift (body) WITH (phrase_index = pairs, positions = off);
SELECT reloptions FROM pg_class WHERE relname IN ('po_pairs', 'po_pairs_off') ORDER BY relname;
SELECT sift.verify('po_pairs', true), sift.verify('po_pairs_off', true);
SET enable_seqscan = off;
SET enable_indexscan = off;
DROP INDEX po_idx;
DROP INDEX po_pairs_off;
SELECT * FROM diffs();
SELECT heap_checks('po', '"red running"') AS pairs_two, heap_checks('po', '"red running red"') AS pairs_three;
RESET enable_seqscan;
RESET enable_indexscan;

-- a partitioned table
CREATE TABLE pt (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (500);
CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (500) TO (2000);
INSERT INTO pt SELECT id, body FROM pf WHERE body IS NOT NULL AND id < 1000;
CREATE INDEX pt_idx ON pt USING sift (body);
SELECT q, (SELECT count(*) FROM pt WHERE body @@> q) AS parts, (SELECT count(*) FROM pt WHERE sift.match(body, q)) AS oracle
FROM qs() q WHERE (SELECT count(*) FROM pt WHERE body @@> q) <> (SELECT count(*) FROM pt WHERE sift.match(body, q));

DROP TABLE po, pf, pt;
DROP FUNCTION qs(), cnt(text, text), oracle(text, text), top(text, text, int), diffs(), heap_checks(text, text);
DROP EXTENSION pg_sift;
