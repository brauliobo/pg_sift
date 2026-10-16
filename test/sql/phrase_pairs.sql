-- phrase_index = pairs: adjacent token pairs are indexed, two-word phrases need no heap recheck,
-- longer ones are narrowed by their pairs. Results and scores equal the plain index and sift.match
CREATE EXTENSION pg_sift;

CREATE TABLE pp (id int PRIMARY KEY, body text);
CREATE TABLE pn (id int PRIMARY KEY, body text);
INSERT INTO pp
SELECT i, concat_ws(' ', w[1 + i % 5], w[1 + (i / 5) % 5], w[1 + (i / 3) % 5], w[1 + (i * 7) % 5], 'x' || i % 9)
FROM generate_series(1, 600) i, LATERAL (SELECT ARRAY['red', 'green', 'blue', 'quick', 'fox'] w) a;
INSERT INTO pp VALUES (601, NULL), (602, ''), (603, 'red'), (604, 'red green red green red');
INSERT INTO pn SELECT * FROM pp;
CREATE INDEX pp_idx ON pp USING sift (body) WITH (phrase_index = pairs);
CREATE INDEX pn_idx ON pn USING sift (body);
VACUUM ANALYZE pp;
VACUUM ANALYZE pn;

SELECT reloptions FROM pg_class WHERE relname = 'pp_idx';

CREATE FUNCTION qs() RETURNS SETOF text LANGUAGE sql AS $$ SELECT * FROM unnest(ARRAY[
	'"red green"', '"green red"', '"red red"', '"red green red"', '"red green red green"', '"quick fox x1"',
	'"RED   Green"', '"red green" AND blue', '"red green" OR "blue quick"', '"red green" NOT blue',
	'blue NOT "red green"', '"red green" NOT "green blue"', '"nothing here"', '"red nothing"',
	'"red green"^3 OR fox', '"red green" "blue quick" fox', '(fox OR quick) "red green"',
	're* "green blue"', '/gr.*/ OR "fox quick"', 'rad~1 "red green"', '"red"', '"x1 red"']) $$;

-- counts through the index equal the plain index and the document-level evaluation
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
SELECT q, cnt('pp', q) AS pairs, cnt('pn', q) AS plain, oracle('pn', q) AS oracle FROM qs() q
WHERE cnt('pp', q) <> oracle('pn', q) OR cnt('pn', q) <> oracle('pn', q);
SELECT count(*) AS queries, sum(cnt('pp', q)) AS hits FROM qs() q;

-- the same through the bitmap heap scan
SET sift.enable_scan = off;
SET sift.enable_count = off;
SELECT q, cnt('pp', q) AS pairs, oracle('pn', q) AS oracle FROM qs() q WHERE cnt('pp', q) <> oracle('pn', q);
RESET sift.enable_scan;
RESET sift.enable_count;

-- ranked scans return the same documents with bit-identical scores
CREATE FUNCTION top(tbl text, q text, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id || ''='' || s::text, '','' ORDER BY s DESC, id) FROM
		(SELECT id, sift.score(ctid) s FROM %I WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT %s) t', tbl, q, k)
	INTO r;
	RETURN r;
END $$;
SELECT q, top('pp', q, 20) AS pairs, top('pn', q, 20) AS plain FROM qs() q
WHERE top('pp', q, 20) IS DISTINCT FROM top('pn', q, 20);
SELECT q, top('pp', q, 1000) AS pairs, top('pn', q, 1000) AS plain FROM qs() q
WHERE top('pp', q, 1000) IS DISTINCT FROM top('pn', q, 1000);
SELECT top('pp', '"red green"', 3), top('pp', '"red green red" OR fox', 3);

-- a two-word phrase is exact in the index and reads no heap page; a longer one is rechecked
CREATE FUNCTION heap_checks(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON)
		SELECT count(*) FROM %I WHERE body @@> %L', tbl, q) INTO j;
	RETURN (SELECT (n ->> 'Heap Checks')::bigint FROM jsonb_path_query(j, 'strict $.**') n WHERE n ? 'Heap Checks' LIMIT 1);
END $$;
SELECT heap_checks('pp', '"red green"') AS pairs_two, heap_checks('pn', '"red green"') > 0 AS plain_two,
	heap_checks('pp', '"red green red"') > 0 AS pairs_three;

-- pair terms are not words: wildcards, regexes and fuzzy never reach them
SELECT (SELECT count(*) FROM pp WHERE body @@> '/.*/') = (SELECT count(*) FROM pn WHERE body @@> '/.*/') AS regex_all;
SELECT (SELECT count(*) FROM pp WHERE body @@> 're*') = (SELECT count(*) FROM pn WHERE body @@> 're*') AS prefix;
SELECT (SELECT count(*) FROM pp WHERE body @@> '*d*') = (SELECT count(*) FROM pn WHERE body @@> '*d*') AS wild;
SELECT (SELECT count(*) FROM pp WHERE body @@> 'rad~2') = (SELECT count(*) FROM pn WHERE body @@> 'rad~2') AS fuzzy;

-- mutable documents get pairs too, deletes and updates stay exact
INSERT INTO pp VALUES (700, 'red green blue'), (701, 'green red'), (702, 'quick fox red green');
INSERT INTO pn SELECT * FROM pp WHERE id >= 700;
DELETE FROM pp WHERE id = 5;
DELETE FROM pn WHERE id = 5;
UPDATE pp SET body = 'blue quick red green' WHERE id = 6;
UPDATE pn SET body = 'blue quick red green' WHERE id = 6;
SELECT q FROM qs() q WHERE cnt('pp', q) <> oracle('pn', q) OR top('pp', q, 1000) IS DISTINCT FROM top('pn', q, 1000);
SELECT sift.promote('pp_idx') IS NOT NULL AS promoted;
SELECT q FROM qs() q WHERE cnt('pp', q) <> oracle('pn', q) OR top('pp', q, 1000) IS DISTINCT FROM top('pn', q, 1000);
SELECT sift.verify('pp_idx', true);

-- the index option is part of the analyzer: switching it needs a REINDEX
ALTER INDEX pn_idx SET (phrase_index = pairs);
SELECT count(*) FROM pn WHERE body @@> '"red green"';
REINDEX INDEX pn_idx;
SELECT count(*) FROM pn WHERE body @@> '"red green"';

DROP FUNCTION qs(), cnt(text, text), oracle(text, text), top(text, text, int), heap_checks(text, text);
DROP TABLE pp, pn;
DROP EXTENSION pg_sift;
