-- ORDER BY <column>, sift.score(ctid) DESC LIMIT n. The score only breaks the ties of the leading key, so a search
-- that wants the first n rows by the column has to score the rows tied with its n-th, not every match: like
-- ORDER BY sift.score(ctid) DESC LIMIT n, where the ranker scores the blocks that can reach the top n and verifies
-- about n candidates. A SiftScan that scores and hands up every match for the Sort above it costs a multiple of the
-- unranked search (a heap check per candidate in each of its searches) and grows with the matches, not with n.
--
-- The check is the rows the index scores ("Candidates" of the ranked scans in EXPLAIN ANALYZE) against n, so that it
-- holds for any plan that scores few rows; the answers are those of ranking every match and sorting afterwards.
CREATE EXTENSION pg_sift;

CREATE FUNCTION scored_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q INTO plan;
	RETURN coalesce((SELECT sum((n ->> 'Candidates')::bigint)
		FROM jsonb_path_query(plan::jsonb, 'strict $.** ? (exists(@."Candidates"))') n), 0);
END $$;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE 'SELECT string_agg(t::text, E''\n'') FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;

-- 20000 items: 'honey' in 2 of 5 names, repeated 1 to 3 times (three levels of score); the names come in pairs
-- that share their suffix, and created_at repeats, so the leading keys have ties; group 1 holds a tenth of the
-- items, group 2 half of them
CREATE TABLE items (id int PRIMARY KEY, name text, created_at timestamp, pad text) WITH (autovacuum_enabled = off);
INSERT INTO items SELECT i, CASE WHEN i % 5 < 2 THEN repeat('honey ', 1 + i % 3) ELSE '' END || 'w' || lpad((i / 2)::text, 5, '0'),
	timestamp '2024-01-01' + (i * 7919 % 10007) * interval '1 minute', repeat('p', 60) FROM generate_series(1, 20000) i;
CREATE TABLE group_items (group_id int, item_id int) WITH (autovacuum_enabled = off);
INSERT INTO group_items SELECT v.g, i FROM generate_series(1, 20000) i, LATERAL (VALUES (1, i % 10 = 0), (2, i % 2 = 0)) v(g, owns) WHERE v.owns;
CREATE INDEX items_sift ON items USING sift (name);
CREATE INDEX group_items_group ON group_items (group_id, item_id);
ANALYZE items;
ANALYZE group_items;

SELECT count(*) AS matches FROM items WHERE name @@> 'honey';

-- the queries: each answered by ranking every match and sorting afterwards (the oracle), and by the plan the planner picks
CREATE TABLE shapes (shape text PRIMARY KEY, q text, oracle text);
INSERT INTO shapes VALUES
	('score', $$SELECT sift.score(ctid) FROM items WHERE name @@> 'honey' ORDER BY sift.score(ctid) DESC LIMIT 5$$,
	 $$SELECT sc FROM (SELECT sift.score(ctid) sc FROM items WHERE name @@> 'honey' ORDER BY sift.score(ctid) DESC) s ORDER BY sc DESC LIMIT 5$$),
	('name, score', $$SELECT id, name, sift.score(ctid) FROM items WHERE name @@> 'honey' ORDER BY name, sift.score(ctid) DESC, id LIMIT 5$$,
	 $$SELECT id, name, sc FROM (SELECT id, name, sift.score(ctid) sc FROM items WHERE name @@> 'honey' ORDER BY sift.score(ctid) DESC) s ORDER BY name, sc DESC, id LIMIT 5$$),
	('created_at desc, score', $$SELECT id, name, sift.score(ctid) FROM items WHERE name @@> 'honey' ORDER BY created_at DESC, sift.score(ctid) DESC, id LIMIT 5$$,
	 $$SELECT id, name, sc FROM (SELECT id, name, created_at, sift.score(ctid) sc FROM items WHERE name @@> 'honey' ORDER BY sift.score(ctid) DESC) s ORDER BY created_at DESC, sc DESC, id LIMIT 5$$),
	('name, score, group', $$SELECT i.id, i.name, sift.score(i.ctid) FROM items i JOIN group_items g ON g.item_id = i.id WHERE g.group_id = 1 AND i.name @@> 'honey' ORDER BY i.name, sift.score(i.ctid) DESC, i.id LIMIT 5$$,
	 $$SELECT id, name, sc FROM (SELECT i.id, i.name, sift.score(i.ctid) sc FROM items i JOIN group_items g ON g.item_id = i.id WHERE g.group_id = 1 AND i.name @@> 'honey' ORDER BY sift.score(i.ctid) DESC) s ORDER BY name, sc DESC, id LIMIT 5$$),
	('name, score, big group', $$SELECT i.id, i.name, sift.score(i.ctid) FROM items i JOIN group_items g ON g.item_id = i.id WHERE g.group_id = 2 AND i.name @@> 'honey' ORDER BY i.name, sift.score(i.ctid) DESC, i.id LIMIT 5$$,
	 $$SELECT id, name, sc FROM (SELECT i.id, i.name, sift.score(i.ctid) sc FROM items i JOIN group_items g ON g.item_id = i.id WHERE g.group_id = 2 AND i.name @@> 'honey' ORDER BY sift.score(i.ctid) DESC) s ORDER BY name, sc DESC, id LIMIT 5$$);

-- the answers are the oracle's
SELECT shape, answer(q) = answer(oracle) AS same_answer FROM shapes ORDER BY shape;

-- scoring the rows tied with the n-th by the leading key is a few hundred rows of the 8000 matches; ranking them all is not
SELECT shape, scored_rows(q) <= 500 AS scores_few_rows FROM shapes ORDER BY shape;

DROP FUNCTION scored_rows(text), answer(text);
DROP TABLE shapes, group_items, items;
DROP EXTENSION pg_sift;
