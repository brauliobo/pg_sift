-- A statement the hook plans as it is and pulled up (one plan loses, and its memory is deleted) leaves the tree of the statement that called the planner as it was:
-- CREATE TABLE AS plans its query in place and the event triggers (the extension has some) copy the statement afterwards, which failed with "unrecognized node type"
-- the first time a statement was planned whose pulled-up plan won. The table has the rows of the normal plan, for a dimension without rows and for one with.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE ce_dim (category text, tag text);
CREATE TABLE ce_fact (id int PRIMARY KEY, source text, v int) WITH (autovacuum_enabled = false);
INSERT INTO ce_fact SELECT g, 'c' || g % 7, g % 100 FROM generate_series(1, 100000) g;
CREATE INDEX ce_fact_col ON ce_fact USING sift_col (id, source, v);
VACUUM (FREEZE, ANALYZE) ce_fact, ce_dim;

CREATE TABLE shapes (slug text, sql text);
INSERT INTO shapes VALUES
 ('empty_dim_count_by_tag', 'SELECT m.tag, count(o.id) AS orders, count(*) AS rows FROM ce_dim m LEFT JOIN ce_fact o ON o.source = m.category GROUP BY m.tag'),
 ('dim_count_by_tag', 'SELECT m.tag, count(o.id) AS orders, sum(o.v) AS total FROM ce_dim m LEFT JOIN ce_fact o ON o.source = m.category GROUP BY m.tag');

CREATE FUNCTION made(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	DROP TABLE IF EXISTS ce_made;
	EXECUTE 'CREATE TEMP TABLE ce_made AS ' || q;
	SELECT coalesce(string_agg(t::text, ';' ORDER BY t::text), '') INTO r FROM ce_made t;
	RETURN r;
END $$;

SELECT slug, made(sql, true) = made(sql, false) AS same_rows FROM shapes ORDER BY slug;
INSERT INTO ce_dim VALUES ('c1', 'one'), ('c2', 'two'), ('c9', 'none');
ANALYZE ce_dim;
SELECT slug, made(sql, true) = made(sql, false) AS same_rows FROM shapes ORDER BY slug;

DROP FUNCTION made(text, bool);
DROP TABLE shapes, ce_dim, ce_fact;
DROP EXTENSION pg_sift;
