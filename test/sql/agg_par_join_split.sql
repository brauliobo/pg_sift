-- The participants of a scan with workers that takes rows of a unit each make the entry of a key of a lookup join that they meet (its id and rows in the table), so the
-- work of the keys grows with the participants and the workers do not shorten a scan of keys that repeat. A scan of a join with one plain key splits the rows by that key
-- instead: each participant reads every unit and takes the rows whose key is in its share, so each key is met by one of them. The rows are those of the normal plan,
-- of the scan alone and of both splits, for children with a NULL key, children whose parent is gone, parents with none and keys that repeat, with the leader taking the
-- shares of workers that did not start.
CREATE EXTENSION pg_sift;

SET client_min_messages = warning;
SET sift.agg_preagg = off;

CREATE TABLE jp_parent (id int PRIMARY KEY, big bigint, grp int, state text, rank int) WITH (autovacuum_enabled = off);
INSERT INTO jp_parent SELECT g, g::bigint * 1000003, g % 40, (ARRAY['ca', 'ny', 'tx', 'wa', 'fl'])[1 + g % 5], g % 100 FROM generate_series(1, 50000) g;
CREATE TABLE jp_child (id int PRIMARY KEY, parent_id int, parent_big bigint, kind text, at timestamp, amount numeric(8, 2)) WITH (autovacuum_enabled = off, parallel_workers = 4);
INSERT INTO jp_child
SELECT g, k, k::bigint * 1000003, (ARRAY['a', 'b', 'c', 'd'])[1 + g % 4], timestamp '2024-01-01' + (g % 365) * interval '1 day', (g % 1000)::numeric
FROM (SELECT g, CASE WHEN g % 97 = 0 THEN NULL WHEN g % 53 = 0 THEN 60000 + g % 1000 WHEN g % 11 = 0 THEN 1 + g % 5 ELSE 1 + (g::bigint * 7919) % 45000 END AS k FROM generate_series(1, 1000000) g) c;
CREATE INDEX jp_parent_col ON jp_parent USING sift_col (id, big, grp, state, rank);
CREATE INDEX jp_child_col ON jp_child USING sift_col (id, parent_id, parent_big, kind, at, amount);
VACUUM (FREEZE, ANALYZE) jp_parent, jp_child;

CREATE TABLE jp_q (n int, sql text);
INSERT INTO jp_q VALUES
 (1, 'SELECT p.grp, c.kind, count(*) AS joined, count(c.id) AS kids FROM jp_parent p LEFT JOIN jp_child c ON c.parent_id = p.id AND c.at >= timestamp ''2024-07-01'' GROUP BY 1, 2'),
 (2, 'SELECT p.grp, count(*) AS parents FROM jp_parent p LEFT JOIN jp_child c ON c.parent_id = p.id AND c.kind = ''d'' WHERE c.id IS NULL GROUP BY 1'),
 (3, 'SELECT p.state, count(c.id) AS kids, count(DISTINCT p.id) AS parents, sum(c.amount) AS amount FROM jp_parent p LEFT JOIN jp_child c ON c.parent_id = p.id AND c.kind = ''b'' WHERE p.rank >= 50 GROUP BY 1'),
 (4, 'SELECT p.grp, count(c.id) AS kids, count(*) AS joined FROM jp_child c RIGHT JOIN jp_parent p ON c.parent_id = p.id GROUP BY 1'),
 (5, 'SELECT (p.id IS NULL) AS orphan, (c.id IS NULL) AS childless, count(*) AS joined FROM jp_parent p FULL JOIN jp_child c ON c.parent_id = p.id GROUP BY 1, 2'),
 (6, 'SELECT p.grp, count(*) AS n, sum(c.amount) AS amount, count(DISTINCT c.kind) AS kinds FROM jp_child c JOIN jp_parent p ON p.id = c.parent_id GROUP BY 1'),
 (7, 'SELECT p.state, count(*) AS n, min(c.id) AS lo, max(c.amount) AS hi FROM jp_child c LEFT JOIN jp_parent p ON p.id = c.parent_id GROUP BY 1'),
 (8, 'SELECT p.grp, count(c.id) AS kids FROM jp_parent p LEFT JOIN jp_child c ON c.parent_big = p.big GROUP BY 1'),
 (9, 'SELECT c.kind, count(*) AS n FROM jp_child c WHERE EXISTS (SELECT 1 FROM jp_parent p WHERE p.id = c.parent_id AND p.rank < 20) GROUP BY 1');

-- how the workers share a scan the plan gives them: by the key of a join, or the rows
CREATE FUNCTION jp_set(mode text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', (mode <> 'normal')::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', CASE WHEN mode IN ('normal', 'scan') THEN '0' ELSE '3' END, true);
	PERFORM set_config('max_parallel_workers', CASE WHEN mode = 'join_two' THEN '2' ELSE '8' END, true);
	PERFORM set_config('sift.test_split', CASE WHEN mode IN ('join', 'join_two') THEN '2' WHEN mode = 'rows' THEN '1' ELSE '0' END, true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	PERFORM set_config('sift.agg_par_min_rows', '0', true);
	PERFORM set_config('sift.agg_force', (mode <> 'normal')::text, true);
END $$;

CREATE FUNCTION jp_plan(q text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal plan';
BEGIN
	PERFORM jp_set(mode);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'one process'; END IF;
		IF l ~ 'Workers Planned: [1-9]' THEN r := 'rows split'; END IF;
		IF l ~ 'Rows Split By: ' THEN r := 'rows split by the key of a join'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION jp_answer(q text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM jp_set(mode);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;

SELECT n, jp_plan(sql, 'scan') AS scan, jp_plan(sql, 'rows') AS rows, jp_plan(sql, 'join') AS by_key
FROM jp_q ORDER BY n;

-- the rows of every statement under every way of sharing the scan equal those of the normal plan
SELECT n, jp_answer(sql, 'scan') = want AS scan, jp_answer(sql, 'rows') = want AS rows, jp_answer(sql, 'join') = want AS by_key, jp_answer(sql, 'join_two') = want AS leader_takes_shares
FROM (SELECT n, sql, jp_answer(sql, 'normal') AS want FROM jp_q) q ORDER BY n;

-- by cost the read of the key column that every participant repeats in a split by the key is dearer than the keys the participants repeat at this size
-- (the split by the key wins where the scan has millions of rows)
SELECT jp_plan(q, 'default') AS plan FROM (VALUES
 ('SELECT p.grp, c.kind, count(*) FROM jp_parent p LEFT JOIN jp_child c ON c.parent_id = p.id GROUP BY 1, 2')) v (q);

DROP FUNCTION jp_set(text), jp_plan(text, text), jp_answer(text, text);
DROP TABLE jp_q, jp_child, jp_parent;
DROP EXTENSION pg_sift;
