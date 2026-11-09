-- A statement driven by the parent table with a condition on its children (EXISTS, IN, an anti join by a left join, a left, right or full join) and a GROUP BY of a
-- parent column is answered by the scan of the parent with the child as its lookup table when both tables have a sift_col index. The answers are those of the normal
-- plan, for children with a NULL key, children whose parent is gone and parents with none.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE parent (id int PRIMARY KEY, grp int, state text, rank int) WITH (autovacuum_enabled = false);
INSERT INTO parent SELECT g, g % 40, (ARRAY['ca', 'ny', 'tx', 'wa', 'fl'])[1 + g % 5], g % 100 FROM generate_series(1, 20000) g;
CREATE TABLE child (id int PRIMARY KEY, parent_id int, kind text, at timestamp, amount numeric(8, 2)) WITH (autovacuum_enabled = false);
INSERT INTO child SELECT g, CASE WHEN g % 97 = 0 THEN NULL WHEN g % 53 = 0 THEN 30000 + g % 1000 ELSE 1 + (g::bigint * 7919) % 18000 END, (ARRAY['a', 'b', 'c', 'd'])[1 + g % 4],
       timestamp '2024-01-01' + (g % 365) * interval '1 day', (g % 1000)::numeric FROM generate_series(1, 400000) g;
SET client_min_messages = warning;
CREATE INDEX parent_col ON parent USING sift_col (id, grp, state, rank);
CREATE INDEX child_col ON child USING sift_col (id, parent_id, kind, at, amount);
CREATE INDEX child_kind_idx ON child (kind);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) parent, child;

CREATE TABLE shapes (slug text, sql text);
INSERT INTO shapes VALUES
 ('exists_big_child_count_by_group', $q$SELECT p.grp, count(*) FROM parent p WHERE EXISTS (SELECT 1 FROM child c WHERE c.parent_id = p.id AND c.amount > 990) GROUP BY p.grp$q$),
 ('in_list_children_sum_by_state', $q$SELECT p.state, count(*), sum(p.rank) FROM parent p WHERE p.id IN (SELECT parent_id FROM child WHERE kind = 'a' AND at >= timestamp '2024-06-01') AND p.rank < 50 GROUP BY p.state$q$),
 ('left_join_childless_parents_by_group', $q$SELECT p.grp, count(*) FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.kind = 'd' WHERE c.id IS NULL GROUP BY p.grp$q$),
 ('left_join_recent_children_by_group_kind', $q$SELECT p.grp, c.kind, count(*), count(c.id) FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.at >= timestamp '2024-07-01' GROUP BY p.grp, c.kind$q$),
 ('left_join_count_distinct_parents_by_state', $q$SELECT p.state, count(c.id), count(DISTINCT p.id), sum(c.amount) FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.kind = 'b' WHERE p.rank >= 50 GROUP BY p.state$q$),
 ('left_join_no_late_children_by_group', $q$SELECT p.grp, count(*) FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.at >= timestamp '2024-12-01' WHERE c.id IS NULL GROUP BY p.grp$q$),
 ('right_join_children_count_by_group', $q$SELECT p.grp, count(c.id), count(*) FROM child c RIGHT JOIN parent p ON c.parent_id = p.id GROUP BY p.grp$q$),
 ('full_join_orphans_and_childless_count', $q$SELECT (p.id IS NULL), (c.id IS NULL), count(*) FROM parent p FULL JOIN child c ON c.parent_id = p.id GROUP BY 1, 2$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION plan(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT slug, plan(sql), answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

DROP FUNCTION answer(text, bool), plan(text);
DROP TABLE shapes, child, parent;
DROP EXTENSION pg_sift;
