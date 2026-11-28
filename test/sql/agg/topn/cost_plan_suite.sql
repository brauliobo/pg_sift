-- The top-N scans. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_topn_chunk_edge
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- The tuples of a chunk whose first sort key comes after that of the last kept tuple of their group are dropped on that key alone (agg/gen_run.c): a tuple that ties with the edge on the
-- first key, a NULL or a NaN key (NULLS FIRST and LAST, ascending and descending), a second key that breaks the tie and a group that appears late give the rows of sift off.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tc (id int PRIMARY KEY, g int NOT NULL, x float8, y int, z int, d date);
INSERT INTO tc SELECT n, CASE WHEN n < 20000 THEN 1 + n % 7 ELSE 1 + n % 40 END,
  CASE WHEN n % 13 = 0 THEN NULL WHEN n % 101 = 0 THEN 'NaN'::float8 ELSE ((n * 37) % 50) / 4.0 END, CASE WHEN n % 17 = 0 THEN NULL ELSE (n * 11) % 9 END, (n * 5) % 6, date '2024-01-01' + (n * 3) % 20
  FROM generate_series(1, 40000) n;
CREATE INDEX tc_col ON tc USING sift_col (id, g, x, y, z, d);
VACUUM (FREEZE, ANALYZE) tc;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('float desc, id',            $q$SELECT g, (array_agg(id ORDER BY x DESC, id))[1:4] FROM tc GROUP BY g$q$),
  ('float desc nulls last, id', $q$SELECT g, (array_agg(id ORDER BY x DESC NULLS LAST, id))[1:4] FROM tc GROUP BY g$q$),
  ('float asc nulls first, id', $q$SELECT g, (array_agg(id ORDER BY x NULLS FIRST, id))[1:4] FROM tc GROUP BY g$q$),
  ('float asc, id',             $q$SELECT g, (array_agg(id ORDER BY x, id))[1:4] FROM tc GROUP BY g$q$),
  ('one key, ties',             $q$SELECT g, (array_agg(y ORDER BY y DESC NULLS LAST))[1:3] FROM tc GROUP BY g$q$),
  ('int key, two keys',         $q$SELECT g, (array_agg(id ORDER BY y DESC NULLS LAST, z, id))[1:5] FROM tc GROUP BY g$q$),
  ('date key',                  $q$SELECT g, (array_agg(id ORDER BY d DESC, id))[1:3] FROM tc GROUP BY g$q$),
  ('rank',                      $q$SELECT * FROM (SELECT g, id, y, rank() OVER (PARTITION BY g ORDER BY y DESC NULLS LAST) r FROM tc) t WHERE r <= 2$q$),
  ('dense_rank',                $q$SELECT * FROM (SELECT g, id, y, dense_rank() OVER (PARTITION BY g ORDER BY y DESC NULLS LAST) r FROM tc) t WHERE r <= 2$q$),
  ('dense_rank, two keys',      $q$SELECT * FROM (SELECT g, id, y, z, dense_rank() OVER (PARTITION BY g ORDER BY y DESC NULLS LAST, z) r FROM tc) t WHERE r <= 3$q$),
  ('row_number, float',         $q$SELECT * FROM (SELECT g, id, x, row_number() OVER (PARTITION BY g ORDER BY x DESC NULLS LAST, id) r FROM tc) t WHERE r <= 3$q$),
  ('filtered',                  $q$SELECT g, (array_agg(id ORDER BY x DESC NULLS LAST, id) FILTER (WHERE id % 3 = 0))[1:4] FROM tc GROUP BY g$q$);
CREATE FUNCTION chunk_check(q text) RETURNS TABLE (engaged bool, same bool) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.plan_engaged_and_answers_same(q) $$;
SELECT q.name, c.* FROM q, LATERAL chunk_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION chunk_check(text);
DROP TABLE q, tc;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_chunk_edge

-- >>> agg_topn_deep_price
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- The price of a deep page (ORDER BY .. LIMIT 100 OFFSET n) of the top-N scan against the time it takes. The heap holds the n rows of the offset and the page and
-- the Limit above throws the offset away, so the time over a page of 100 grows with n (measured on 600,000 rows, scan only: 1.5, 3.1, 9.8 and 19.4 ms over a
-- page of 100 for an offset of 2,000, 5,000, 20,000 and 50,000), whatever the number of rows that match (60,000 or 600,000: within 20%). `measured` is that time in
-- cost units (355 per ms: what the base cost of the scan is worth); the price of the scan over its price at OFFSET 100 must stay within half and twice of it.
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
CREATE TABLE events (id int PRIMARY KEY, x int NOT NULL, ts timestamp NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, (hashint4(g) & 2147483647) % 100, timestamp '2024-01-01' + (hashint4(g + 1) & 2147483647) % 100000000 * interval '1 second', g % 1000
FROM generate_series(1, 600000) g;
CREATE INDEX events_col ON events USING sift_col (x, ts, amount, id) WITH (order_array_columns = off);
VACUUM (FREEZE, ANALYZE) events;
-- the plan without the extension is not an alternative here: its cost is not what is checked
SET enable_sort = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;

CREATE FUNCTION scan_price(bound int, off int) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE line text; p float8;
BEGIN
	FOR line IN EXECUTE format('EXPLAIN SELECT id, amount FROM events WHERE x < %s ORDER BY ts, id LIMIT 100 OFFSET %s', bound, off) LOOP
		IF line ~ 'Custom Scan \(SiftAgg' THEN p := substring(line from 'cost=[0-9.]+\.\.([0-9.]+)')::float8; END IF;
	END LOOP;
	RETURN p;
END $$;

SELECT matching, off, ((scan_price(bound, off) - scan_price(bound, 100)) / measured)::numeric BETWEEN 0.5 AND 2.0 AS priced_within_band
FROM (VALUES (100, 'all', 2000, 530), (100, 'all', 5000, 1090), (100, 'all', 20000, 3480), (100, 'all', 50000, 6870),
             (10, 'a tenth', 2000, 290), (10, 'a tenth', 5000, 850), (10, 'a tenth', 20000, 2580), (10, 'a tenth', 50000, 6340)) m(bound, matching, off, measured)
ORDER BY matching, off;

DROP FUNCTION scan_price(int, int);
DROP TABLE events;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_deep_price

-- >>> agg_topn_part_cost
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- The price of a top-N over a partitioned table that reads one partition's order array: a probe restarted for each row of a join costs more over more partitions (it opens the source
-- of the first partition and prunes the others each time), and the start of a scan that pruning left one partition prices the segments of that partition, not of the whole table.
-- The estimates are compared (never printed): the order must hold at any cost scale. (The start, not the total: the heap visits of a partition's rows are priced from its pages.)
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE FUNCTION pc_make(nm text, parts int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id int NOT NULL, tenant int NOT NULL, ts timestamp NOT NULL, pad text) PARTITION BY HASH (tenant)', nm);
	FOR i IN 0 .. parts - 1 LOOP
		EXECUTE format('CREATE TABLE %I PARTITION OF %I FOR VALUES WITH (MODULUS %s, REMAINDER %s)', nm || '_' || i, nm, parts, i);
	END LOOP;
	EXECUTE format('INSERT INTO %I SELECT i, i %% 100, timestamp ''2025-01-01'' + (i * 7919 %% 7776000) * interval ''1 second'', repeat(''x'', 20) FROM generate_series(1, 20000) i', nm);
	EXECUTE format('CREATE INDEX ON %I USING sift_col (id, tenant, ts) WITH (order_array_columns = ''ts'', order_array_group = ''tenant'')', nm);
END $$;

SELECT pc_make('p4', 4), pc_make('p16', 16), pc_make('p64', 64), pc_make('q2', 2), pc_make('q8', 8);
CREATE TABLE tn AS SELECT n FROM generate_series(0, 99) n;
VACUUM (FREEZE, ANALYZE) p4, p16, p64, q2, q8, tn;

-- the total cost of the plan with the other paths disabled, and whether it is a SiftAgg (agg_force would price it at 0)
CREATE FUNCTION pc_cost(q text, OUT total float8, OUT startup float8, OUT sift bool) LANGUAGE plpgsql AS $$
DECLARE j json; pl text;
BEGIN
	SET LOCAL plan_cache_mode = force_generic_plan; SET LOCAL sift.enable_agg = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	EXECUTE 'EXPLAIN (FORMAT JSON, COSTS on) ' || q INTO j;
	total := (j->0->'Plan'->>'Total Cost')::float8;
	startup := (j->0->'Plan'->>'Startup Cost')::float8;
	sift := false;
	FOR pl IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		sift := sift OR pl LIKE '%Custom Scan (SiftAgg%';
	END LOOP;
END $$;

CREATE FUNCTION pc_lat(t text) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT tn.n, x.ts FROM tn LEFT JOIN LATERAL (SELECT ts FROM %I e WHERE e.tenant = tn.n ORDER BY ts DESC LIMIT 1) x ON true', t) $$;

-- the partitions a probe prunes and opens each time: 4 < 16 < 64, and the scan is a SiftAgg at each
SELECT (a).sift AND (b).sift AND (c).sift AS sift_scans, (a).total < (b).total AND (b).total < (c).total AS dearer_with_partitions
FROM (SELECT pc_cost(pc_lat('p4')) a, pc_cost(pc_lat('p16')) b, pc_cost(pc_lat('p64')) c) s;

-- the same rows in 2 partitions and in 8, the partition chosen by a parameter of a generic plan (nothing is pruned when it is planned): the start prices the rows of one partition (the segments of its array)
UPDATE pg_class SET reltuples = 4000000 WHERE relname ~ '^q2_[0-9]+$';
UPDATE pg_class SET reltuples = 1000000 WHERE relname ~ '^q8_[0-9]+$';
PREPARE pc_q2(int) AS SELECT ts FROM q2 WHERE tenant = $1 ORDER BY ts DESC LIMIT 5;
PREPARE pc_q8(int) AS SELECT ts FROM q8 WHERE tenant = $1 ORDER BY ts DESC LIMIT 5;
SELECT (a).sift AND (b).sift AS sift_scans, (b).startup < (a).startup AS start_prices_the_partition
FROM (SELECT pc_cost('EXECUTE pc_q2(7)') a, pc_cost('EXECUTE pc_q8(7)') b) s;

DEALLOCATE pc_q2;
DEALLOCATE pc_q8;
DROP FUNCTION pc_cost(text), pc_lat(text), pc_make(text, int);
DROP TABLE p4, p16, p64, q2, q8, tn;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_part_cost

-- >>> agg_topn_probe_cost
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- A top-1 probe of the grouped order array for each row of a LATERAL (the latest row of an entity) restarts a scan whose source stays open: 19,500 instructions and 2 buffer pins,
-- 2.4 to 2.7 us a probe over 1,000, 3,000 and 10,000 entities at 100,000 to 1,000,000 rows. Every probe was priced as the first open of the array (28,000 operators and 7,300 for
-- each segment: 88 to 106 units), forty times what it takes and more than the aggregate that reads the same entities. The cost of a probe is checked at 20,000 rows.
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE ev AS SELECT 1 + (i::bigint * 7919 % 200)::int AS ent, now() - (hashint4(i) & 8388607) * interval '1 second' AS ts, i % 1000 AS v FROM generate_series(1, 20000) i;
CREATE TABLE ents AS SELECT g AS id, g % 30 AS grp FROM generate_series(1, 200) g;
CREATE INDEX ev_c ON ev USING sift_col (ent, ts, v) WITH (order_array_columns = 'ts', order_array_group = 'ent');
VACUUM (FREEZE, ANALYZE) ev, ents;

-- the cost of one probe (the Limit's startup) and of the aggregate of the same entities, in the plan the planner chooses
CREATE FUNCTION probe_cost() RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE l text; c float8;
BEGIN
	FOR l IN EXPLAIN SELECT d.id, x.ts FROM ents d LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = d.id ORDER BY ts DESC LIMIT 1) x ON true WHERE d.grp = 3 LOOP
		IF l ~ 'Limit' AND c IS NULL THEN c := substring(l FROM 'cost=([0-9.]+)\.\.')::float8; END IF;
	END LOOP;
	RETURN c;
END $$;
SELECT probe_cost() BETWEEN 1.5 AND 5 AS probe_costs_microseconds;

-- the same rows with the probes and with the normal plan
CREATE FUNCTION probe_rows(push boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT md5(string_agg(concat_ws(%L, d.id, x.ts), %L ORDER BY d.id)) FROM ents d LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = d.id ORDER BY ts DESC LIMIT 1) x ON true WHERE d.grp = 3', ',', ';') INTO r;
	RETURN r;
END $$;

SELECT probe_rows(true) = probe_rows(false) AS same_rows;

DROP FUNCTION probe_cost(), probe_rows(boolean);
DROP TABLE ev, ents;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_probe_cost

-- >>> agg_topn_walk_cost
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- ORDER BY k LIMIT n with a condition on another column: the Top-N scan reads the units, tests the condition on the columns of the index and keeps the best n of the rows that
-- pass, so a condition that most rows pass costs about the order alone, and a rarer one keeps fewer rows (forced scan: 0.76 ms for the order, 0.93 for half the rows, 0.63 for
-- a tenth, 0.46 for a hundredth). The price follows: under twice the order, and falling with the rows that pass.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE feed (id int, k int, q50 int, q10 int, q1 int, gone timestamptz, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE feed ALTER pad SET STORAGE PLAIN;
INSERT INTO feed SELECT i, (i::bigint * 7919) % 60001, (hashint4(i) & 1023 < 512)::int, (hashint4(i + 1) & 1023 < 102)::int, (hashint4(i + 2) & 1023 < 10)::int,
	CASE WHEN i % 50 = 0 THEN now() END, repeat('x', 900) FROM generate_series(1, 60000) i;
CREATE INDEX feed_i ON feed USING sift_col (id, k, q50, q10, q1, gone) WITH (order_array_columns = 'k');
VACUUM (FREEZE, ANALYZE) feed;

CREATE FUNCTION page_cost(cond text) RETURNS float8 LANGUAGE sql AS
$$ SELECT scan_cost FROM sift.explain_why('SELECT id, k FROM feed WHERE ' || cond || ' ORDER BY k LIMIT 20') $$;

SELECT page_cost('q50 = 1') < 2 * page_cost('true') AS half_pass_costs_the_order,
	page_cost('gone IS NULL') < 2 * page_cost('true') AS soft_delete_costs_the_order,
	page_cost('q1 = 1') < page_cost('q10 = 1') AS cost_falls_with_the_rows,
	page_cost('q10 = 1') < page_cost('q50 = 1') AS cost_falls_from_half_to_tenth;

DROP FUNCTION page_cost(text);
DROP TABLE feed;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_walk_cost

-- >>> agg_topn_walk_refused
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
-- The latest row of an entity out of thousands (ORDER BY ts DESC LIMIT 1 WHERE ent = n, alone or in a LATERAL per entity) is not read by walking the order array of ts: the heap row
-- of each entry would test ent, and the statistics' pass rate (one in the entities) asks for more fetches than a walk would make. The Top-N scan reads the units whose zones hold
-- the entity and tests ent on the column of the index (Granules: scanned): it starts no walk.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int, ent int, ts int, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE ev ALTER pad SET STORAGE PLAIN;
INSERT INTO ev SELECT i, (i::bigint * 7919) % 2003, i, repeat('x', 200) FROM generate_series(1, 60000) i;
CREATE INDEX ev_i ON ev USING sift_col (id, ent, ts) WITH (order_array_columns = 'ts');
CREATE TABLE ents AS SELECT g AS id FROM generate_series(1, 20) g;
VACUUM (FREEZE, ANALYZE) ev, ents;

CREATE FUNCTION reads_units(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; units bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
		units := units OR substring(l FROM 'Granules: scanned=([0-9]+)')::int > 0;
	END LOOP;
	RETURN units;
END $$;

SET sift.agg_force = on;
SELECT reads_units('SELECT ts FROM ev WHERE ent = 77 ORDER BY ts DESC LIMIT 1') AS one_entity,
	reads_units('SELECT e.id, x.ts FROM ents e LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = e.id ORDER BY ts DESC LIMIT 1) x ON true') AS per_entity;
RESET sift.agg_force;

-- the same rows with the normal plan
CREATE FUNCTION walk_rows(push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('sift.agg_force', push::text, true);
	EXECUTE 'SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (SELECT e.id, x.ts FROM ents e LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = e.id ORDER BY ts DESC LIMIT 1) x ON true) x' INTO r;
	RETURN r;
END $$;
SELECT walk_rows(true) = walk_rows(false) AS same_rows;

DROP FUNCTION reads_units(text), walk_rows(bool);
DROP TABLE ev, ents;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_walk_refused

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
