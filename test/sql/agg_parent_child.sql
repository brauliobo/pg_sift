-- The statements that go from a parent table to its children (a count of the children for each parent, a filter of the parent with a sum over the children, EXISTS, NOT EXISTS,
-- IN, HAVING on the number of children, a lateral top-1 child, distinct values of a child column, a list of parents with a child filter) over a parent and a child table that
-- each have a sift_col index: the answers are those of the normal plan, whatever plan each side takes. The baseline of the 4.3 connected indexes; the same statements are the
-- bench cases parent_child_* at 1,000,000 child rows of 100,000 parents.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE pc_parent (id int PRIMARY KEY, region int, kind int, name text) WITH (autovacuum_enabled = false);
INSERT INTO pc_parent SELECT i, i % 50, i % 7, 'n' || i FROM generate_series(1, 20000) i;
CREATE TABLE pc_child (id int PRIMARY KEY, parent_id int NOT NULL REFERENCES pc_parent, v int, ts date, flag boolean) WITH (autovacuum_enabled = false);
-- the parents above 18,000 have no child; v is NULL in one row of eleven
INSERT INTO pc_child SELECT i, 1 + (i::bigint * 7919) % 18000, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 1000 END, date '2026-01-01' + i % 365, i % 5 = 0 FROM generate_series(1, 200000) i;
CREATE INDEX pc_parent_col ON pc_parent USING sift_col (id, region, kind, name);
CREATE INDEX pc_child_col ON pc_child USING sift_col (id, parent_id, v, ts, flag);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) pc_parent, pc_child;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT p.id, count(c.id) FROM pc_parent p LEFT JOIN pc_child c ON c.parent_id = p.id GROUP BY p.id ORDER BY 2 DESC, 1 LIMIT 10$q$),
 (2, $q$SELECT p.region, count(c.id), count(c.v) FROM pc_parent p LEFT JOIN pc_child c ON c.parent_id = p.id GROUP BY 1$q$),
 (3, $q$SELECT p.kind, sum(c.v) FROM pc_parent p JOIN pc_child c ON c.parent_id = p.id WHERE p.region = 7 GROUP BY 1$q$),
 (4, $q$SELECT count(*) FROM pc_parent p WHERE EXISTS (SELECT 1 FROM pc_child c WHERE c.parent_id = p.id AND c.flag)$q$),
 (5, $q$SELECT count(*) FROM pc_parent p WHERE NOT EXISTS (SELECT 1 FROM pc_child c WHERE c.parent_id = p.id AND c.v = 7 AND c.flag)$q$),
 (6, $q$SELECT p.region, sum(c.v) FROM pc_parent p LEFT JOIN pc_child c ON c.parent_id = p.id AND c.flag AND c.ts >= date '2026-06-01' GROUP BY 1$q$),
 (7, $q$SELECT count(*) FROM pc_parent p WHERE p.id IN (SELECT parent_id FROM pc_child WHERE v = 7)$q$),
 (8, $q$SELECT count(*) FROM (SELECT p.id FROM pc_parent p JOIN pc_child c ON c.parent_id = p.id GROUP BY p.id HAVING count(*) > 10) s$q$),
 (9, $q$SELECT count(*) FROM pc_parent p, LATERAL (SELECT c.v FROM pc_child c WHERE c.parent_id = p.id ORDER BY c.ts DESC, c.id LIMIT 1) l WHERE p.kind = 3$q$),
 (10, $q$SELECT p.kind, count(DISTINCT c.v) FROM pc_parent p JOIN pc_child c ON c.parent_id = p.id GROUP BY 1$q$),
 (11, $q$SELECT p.region, count(*) FROM pc_parent p WHERE EXISTS (SELECT 1 FROM pc_child c WHERE c.parent_id = p.id AND c.v = 7) GROUP BY 1$q$),
 (12, $q$SELECT p.id, p.name, count(*) FROM pc_parent p JOIN pc_child c ON c.parent_id = p.id WHERE p.kind = 3 AND c.flag GROUP BY 1, 2 ORDER BY 3 DESC, 1 LIMIT 20$q$);

CREATE FUNCTION answer(sql text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', (mode <> 'off')::text, true), set_config('sift.agg_force', (mode = 'force')::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

-- by the planner's choice, and forced to the scan where a scan can take the statement
SELECT n, answer(sql, 'on') = answer(sql, 'off') AS same_answers, answer(sql, 'force') = answer(sql, 'off') AS same_answers_forced FROM shapes ORDER BY n;

DROP FUNCTION answer(text, text);
DROP TABLE shapes, pc_child, pc_parent;
DROP EXTENSION pg_sift;
