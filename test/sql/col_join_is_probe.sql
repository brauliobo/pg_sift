-- The rows of 5 keys joined to a wide table (40 integer columns, 20,000 rows) that has a sift_col index and no b-tree: a probe for each key through the plain index scan (the heap
-- row of the address the point directory gives) takes 1.4 ms for 1,000 keys of 100,000 rows where the index-only scan takes 17 ms (its units are read) and the hash join over the heap 9.7.
-- PostgreSQL builds the plain path only where the index-only one is not possible, and the plain path beside an index-only one was priced for one probe, not the keys of the join, so
-- the nested loop never won. It is priced for the probes of the join, and the nested loop with the plain index scan is the plan.
-- The same join written as a LATERAL (the sub-select a parent query passes the key to) was priced as a correlated subplan, at 0.9 of the sequential scan of the table for each
-- probe (6,160 units against 22 for a constant key, 190 M instructions and 27 ms for 1,000 keys of 300,000 rows against 20 M and 4.3 ms with the plain scan): the loops of a join are its rows.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

DO $$
DECLARE cols text := ''; defs text := ''; i int;
BEGIN
	FOR i IN 1..40 LOOP
		cols := cols || format(', (g * %s %% 1000)::int AS c%s', i + 1, i);
		IF i <= 20 THEN defs := defs || format(', c%s', i); END IF;
	END LOOP;
	EXECUTE 'CREATE TABLE wide AS SELECT (g::bigint * 7919 % 20011)::int AS k, g AS n' || cols || ' FROM generate_series(1, 20000) g';
	EXECUTE 'CREATE INDEX wide_c ON wide USING sift_col (k, n' || defs || ')';
END $$;
CREATE TABLE kk AS SELECT (i::bigint * 7919 % 20011)::int AS k FROM generate_series(1, 5) i;
VACUUM (FREEZE, ANALYZE) wide, kk;

CREATE FUNCTION join_nodes(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Nested Loop|Hash Join|Merge Join)' THEN r := r || regexp_replace(trim(l), '^-> *', '') || '; '; END IF;
		IF l ~ ' on wide' THEN r := r || regexp_replace(trim(l), '^-> *| on wide.*', '', 'g'); END IF;
	END LOOP;
	RETURN r;
END $$;
SELECT join_nodes('SELECT t.k, t.n FROM kk JOIN wide t ON t.k = kk.k ORDER BY t.n') AS plan;
CREATE FUNCTION probe_cost(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
	RETURN (SELECT max((n->>'Total Cost')::float8) FROM jsonb_path_query(p, '$.**.Plans[*] ? (@."Relation Name" == "wide")') n);
END $$;
SELECT probe_cost('SELECT count(*), sum(x.n) FROM kk JOIN LATERAL (SELECT n FROM wide t WHERE t.k = kk.k OFFSET 0) x ON true') < 100 AS lateral_probe_is_a_lookup;

CREATE FUNCTION same_rows(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (%s) t', q) INTO a;
	PERFORM set_config('sift.enable_col_scan', 'off', true);
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (%s) t', q) INTO b;
	RETURN a = b;
END $$;
SELECT same_rows('SELECT t.k, t.n FROM kk JOIN wide t ON t.k = kk.k ORDER BY t.n') AS same_rows;
SELECT same_rows('SELECT kk.k, x.n FROM kk JOIN LATERAL (SELECT n FROM wide t WHERE t.k = kk.k OFFSET 0) x ON true') AS same_lateral_rows;

DROP FUNCTION join_nodes(text), same_rows(text), probe_cost(text);
DROP TABLE wide, kk;
DROP EXTENSION pg_sift;
