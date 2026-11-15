-- count(x) of a column of a joined table needs whether x is NULL, not the id of its value: the scan keeps a flag for each joined tuple, with no table of the values
-- (a column with a value in every row made a table of the size of the lookup table's rows). The aggregates that do need the ids (array_agg, count(DISTINCT); not min, max and sum, which are added tuple by tuple) of such a
-- column cost more than those of a column of a few values. The answers are those of the normal plan, the rows of the parents with no child and the NULLs included. count(x + y) has PostgreSQL evaluate the sum for each joined tuple, which costs more than count(x).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE parent (id int PRIMARY KEY, region int) WITH (autovacuum_enabled = false);
INSERT INTO parent SELECT g, g % 11 FROM generate_series(1, 30000) g;
CREATE TABLE child (id int PRIMARY KEY, parent_id int NOT NULL REFERENCES parent, v int, tag text) WITH (autovacuum_enabled = false);
-- the parents 24001 to 30000 have no child; v is NULL in one row of seven, and has 1000 values
INSERT INTO child SELECT g, 1 + (g::bigint * 7919) % 24000, CASE WHEN g % 7 = 0 THEN NULL ELSE g % 1000 END, CASE WHEN g % 5 = 0 THEN NULL ELSE 't' || g % 13 END
FROM generate_series(1, 300000) g;
CREATE INDEX parent_col ON parent USING sift_col (id, region);
CREATE INDEX child_col ON child USING sift_col (id, parent_id, v, tag);
VACUUM (FREEZE, ANALYZE) parent, child;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes
SELECT row_number() OVER (), format(q, a) FROM (VALUES ('count(c.id)'), ('count(c.v)'), ('count(c.tag)'), ('count(c.v + 1)'), ('count(c.tag) FILTER (WHERE c.v > 500)')) a (a),
 (VALUES ($q$SELECT p.region, %s FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY 1$q$),
         ($q$SELECT p.id, %s FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10$q$),
         ($q$SELECT p.region, %s FROM child c JOIN parent p ON c.parent_id = p.id WHERE p.id > 100 GROUP BY 1$q$)) q (q);

CREATE FUNCTION answer(sql text, force bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.agg_force', force::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION plan(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.agg_force', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION scan_cost(agg text) RETURNS float8 LANGUAGE sql AS $$
	SELECT min(scan_cost) FROM sift.explain_why(format($q$SELECT p.id, %s FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10$q$, agg)) WHERE arm = 'single'
$$;

-- the lookup join is what is priced: the child grouped by its key first (sift.agg_child_first) is another plan of these statements
SET sift.agg_child_first = off;
SELECT n, sql, plan(sql), answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY n;

-- the scan's price: a count needs no ids, and the ids of 300,000 values cost more than those of 1,000
SELECT scan_cost('count(DISTINCT c.id)') > 1.5 * scan_cost('count(DISTINCT c.v)') AS wide_arg_costs_more, scan_cost('count(c.id)') < scan_cost('array_agg(c.id)') / 2 AS count_costs_less,
       scan_cost('count(c.v + c.id)') > 1.5 * scan_cost('count(c.v)') AS expr_count_costs_more;

DROP FUNCTION plan(text), answer(text, bool), scan_cost(text);
DROP TABLE shapes, child, parent;
DROP EXTENSION pg_sift;
