-- Why a pushed scan has no workers, one statement for each rule that decides it (EXPLAIN VERBOSE: Serial Because): the aggregate kinds whose partial states the
-- leader cannot merge, a group key that shows a spelling of equal values, the joins whose lookup tables the process of the scan loads, a statement or sort
-- expression that is not parallel safe. 'workers' when nothing keeps them from it; 'none given' when the scan has none and no rule is named.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE f (id int, g int, k int, x numeric, y float8, t text, d_id int) WITH (autovacuum_enabled = false);
INSERT INTO f SELECT i, i % 7, i % 5, (i % 1001)::numeric / 8, (i % 997) / 8.0, 'v' || i % 50, i % 30 FROM generate_series(1, 60000) i;
CREATE TABLE d (id int PRIMARY KEY, name text, w numeric);
INSERT INTO d SELECT i, 'n' || i % 9, (i % 13)::numeric FROM generate_series(0, 29) i;
CREATE INDEX f_col ON f USING sift_col (id, g, k, x, y, t, d_id);
VACUUM (FREEZE, ANALYZE) f;
ANALYZE d;
CREATE FUNCTION unsafe_k(i int) RETURNS int LANGUAGE sql IMMUTABLE PARALLEL UNSAFE AS 'SELECT i';
CREATE FUNCTION restricted_k(i int) RETURNS int LANGUAGE sql IMMUTABLE PARALLEL RESTRICTED AS 'SELECT i';

CREATE FUNCTION scan_workers(q text, preagg bool DEFAULT false) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'no scan';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_force', 'on', true);
	PERFORM set_config('sift.agg_preagg', preagg::text, true);
	FOR l IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'none given'; END IF;
		IF l LIKE '%Workers Planned%' AND r <> 'no scan' THEN RETURN 'workers'; END IF;
		IF l LIKE '%Serial Because%' THEN RETURN trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE TABLE shapes (n serial, name text, q text);
INSERT INTO shapes (name, q) VALUES
	('plain',              'SELECT g, count(*), sum(y) FROM f GROUP BY g'),
	('generic and float',  'SELECT g, sum(y), string_agg(k::text, '','') FROM f GROUP BY g'),
	('spelling of a key',  'SELECT x, count(*) FROM f GROUP BY x'),
	('joined sum',         'SELECT g, sum(d.w) FROM f JOIN d ON d.id = f.d_id GROUP BY g'),
	('joined min',         'SELECT g, min(d.name) FROM f JOIN d ON d.id = f.d_id GROUP BY g'),
	('expression sum',     'SELECT g, sum(d.w + f.y::numeric) FROM f JOIN d ON d.id = f.d_id GROUP BY g'),
	('expression min',     'SELECT g, min(d.w * 2) FROM f JOIN d ON d.id = f.d_id GROUP BY g'),
	('expression float',   'SELECT g, sum(d.w::float8 + 1) FROM f JOIN d ON d.id = f.d_id GROUP BY g'),
	('expression count',   'SELECT g, count(d.w + 1) FROM f LEFT JOIN d ON d.id = f.d_id GROUP BY g'),
	('outer join',         'SELECT g, count(*) FROM f LEFT JOIN d ON d.id = f.d_id GROUP BY g'),
	('unsafe in a sum',    'SELECT g, sum(unsafe_k(k)) FROM f GROUP BY g'),
	('unsafe in a filter', 'SELECT g, count(*) FROM f WHERE unsafe_k(k) > 1 GROUP BY g'),
	('unsafe in a key',    'SELECT unsafe_k(g), count(*) FROM f GROUP BY 1'),
	('restricted filter',  'SELECT g, count(*) FROM f WHERE restricted_k(k) > 1 GROUP BY g'),
	('top-n',              'SELECT id FROM f ORDER BY y DESC LIMIT 10'),
	('top-n unsafe key',   'SELECT id FROM f ORDER BY unsafe_k(k), id LIMIT 10'),
	('top-n joined',       'SELECT f.id FROM f JOIN d ON d.id = f.d_id ORDER BY f.y DESC LIMIT 10'),
	('top-n with a total', 'SELECT g, count(*) FROM f GROUP BY g ORDER BY 2 DESC LIMIT 3');
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT n, name, scan_workers(q) FROM shapes ORDER BY n;
-- the pre-aggregates of the index answer a count by group without a scan of the rows
SELECT n, name, scan_workers(q, true) FROM shapes WHERE n IN (1, 11, 18) ORDER BY n;
-- no workers asked for: no reason is printed
SET max_parallel_workers_per_gather = 0;
SELECT n, name, scan_workers(q) FROM shapes WHERE n <= 4 ORDER BY n;

DROP FUNCTION scan_workers(text, bool), unsafe_k(int), restricted_k(int);
DROP TABLE shapes, f, d;
DROP EXTENSION pg_sift;
