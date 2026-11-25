-- track_changes on partitions, in the first statement of a new backend that loads the library while it runs (INSERT, COPY and MERGE reach a partition, and its index, only when they route a row),
-- with what runs inside the statement: BEFORE ROW triggers and functions that read the table through the sift_col index before the statement is over (the counts must be the heap's: the
-- versions the statement ended are not in the log yet), data-modifying CTEs, MERGE, COPY with reads in the same transaction, savepoints, prepared statements, attach and detach in a
-- transaction and the parent of an inheritance tree. Every case runs as the first statement of a new backend, in the partitioned kinds: hash, range, list and two levels.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE trlog (seq serial, ev text, n_sift text, n_heap text);
-- the count, the sum and the distinct keys of t, by the sift_col index (its read is priced in) and by the heap alone
CREATE FUNCTION read_sift(t regclass) RETURNS text LANGUAGE plpgsql VOLATILE SET sift.enable_agg = on SET seq_page_cost = 10000 AS $$
DECLARE r text;
BEGIN EXECUTE format('SELECT count(*) || ''/'' || coalesce(sum(v), 0) || ''/'' || count(DISTINCT id) FROM %s', t) INTO r; RETURN r; END $$;
CREATE FUNCTION read_heap(t regclass) RETURNS text LANGUAGE plpgsql VOLATILE SET sift.enable_agg = off SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off AS $$
DECLARE r text;
BEGIN EXECUTE format('SELECT count(*) || ''/'' || coalesce(sum(v), 0) || ''/'' || count(DISTINCT id) FROM %s', t) INTO r; RETURN r; END $$;
CREATE FUNCTION log_reads(ev text, t regclass) RETURNS void LANGUAGE sql AS $$ INSERT INTO trlog (ev, n_sift, n_heap) VALUES (ev, read_sift(t), read_heap(t)) $$;
CREATE FUNCTION log_trigger() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN PERFORM log_reads(TG_ARGV[0] || ' ' || TG_WHEN || ' ' || TG_LEVEL || ' ' || TG_OP, TG_ARGV[0]::regclass); RETURN NEW; END $$;
CREATE FUNCTION log_value(ev text, t regclass) RETURNS int LANGUAGE plpgsql AS $$ BEGIN PERFORM log_reads(ev, t); RETURN 1; END $$;
-- what the statements read inside, by case: how many reads, and how many differ from the heap's
CREATE FUNCTION log_summary(OUT ev text, OUT reads bigint, OUT differing bigint) RETURNS SETOF record LANGUAGE sql AS $$
	SELECT l.ev, count(*), count(*) FILTER (WHERE n_sift <> n_heap) FROM trlog l GROUP BY l.ev ORDER BY l.ev $$;
-- the leaves of t whose sift_col read is not the heap's, and the same for the table as a whole
CREATE FUNCTION leaves_differing(t regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT sum(sift_helpers.groups_differences_sift_vs_heap(relid)) + sift_helpers.groups_differences_sift_vs_heap(t) FROM pg_partition_tree(t) WHERE isleaf $$;

CREATE TABLE th (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) PARTITION BY HASH (id);
CREATE TABLE th0 PARTITION OF th FOR VALUES WITH (MODULUS 3, REMAINDER 0) WITH (autovacuum_enabled = false);
CREATE TABLE th1 PARTITION OF th FOR VALUES WITH (MODULUS 3, REMAINDER 1) WITH (autovacuum_enabled = false);
CREATE TABLE th2 PARTITION OF th FOR VALUES WITH (MODULUS 3, REMAINDER 2) WITH (autovacuum_enabled = false);
CREATE TABLE tr (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE tr0 PARTITION OF tr FOR VALUES FROM (0) TO (2400) WITH (autovacuum_enabled = false);
CREATE TABLE tr1 PARTITION OF tr FOR VALUES FROM (2400) TO (4800) WITH (autovacuum_enabled = false);
CREATE TABLE tr2 PARTITION OF tr FOR VALUES FROM (4800) TO (100000) WITH (autovacuum_enabled = false);
CREATE TABLE tl (id int NOT NULL, g int NOT NULL, v int NOT NULL, PRIMARY KEY (id, g)) PARTITION BY LIST (g);
CREATE TABLE tl0 PARTITION OF tl FOR VALUES IN (0, 1, 2) WITH (autovacuum_enabled = false);
CREATE TABLE tl1 PARTITION OF tl FOR VALUES IN (3, 4) WITH (autovacuum_enabled = false);
CREATE TABLE tl2 PARTITION OF tl FOR VALUES IN (5, 6) WITH (autovacuum_enabled = false);
CREATE TABLE tm (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE tm0 PARTITION OF tm FOR VALUES FROM (0) TO (4500) PARTITION BY HASH (id);
CREATE TABLE tm00 PARTITION OF tm0 FOR VALUES WITH (MODULUS 2, REMAINDER 0) WITH (autovacuum_enabled = false);
CREATE TABLE tm01 PARTITION OF tm0 FOR VALUES WITH (MODULUS 2, REMAINDER 1) WITH (autovacuum_enabled = false);
CREATE TABLE tm1 PARTITION OF tm FOR VALUES FROM (4500) TO (100000) PARTITION BY HASH (id);
CREATE TABLE tm10 PARTITION OF tm1 FOR VALUES WITH (MODULUS 2, REMAINDER 0) WITH (autovacuum_enabled = false);
CREATE TABLE tm11 PARTITION OF tm1 FOR VALUES WITH (MODULUS 2, REMAINDER 1) WITH (autovacuum_enabled = false);
INSERT INTO th SELECT i * 3, i % 7, i FROM generate_series(0, 2399) i;
INSERT INTO tr SELECT i * 3, i % 7, i FROM generate_series(0, 2399) i;
INSERT INTO tl SELECT i * 3, i % 7, i FROM generate_series(0, 2399) i;
INSERT INTO tm SELECT i * 3, i % 7, i FROM generate_series(0, 2399) i;
SELECT format('CREATE INDEX %s_c ON %s USING sift_col (id, g, v)', c, c) FROM unnest(ARRAY['th0', 'th1', 'th2', 'tr0', 'tr1', 'tr2', 'tl0', 'tl1', 'tl2', 'tm00', 'tm01', 'tm10', 'tm11']) c \gexec
-- the triggers log what the statement reads at each row, from the partition and from the table the statement names
SELECT format('CREATE TRIGGER trb BEFORE INSERT OR UPDATE ON %s FOR EACH ROW EXECUTE FUNCTION log_trigger(%s)', leaf, quote_literal(t))
FROM (VALUES ('th', 'th0'), ('th', 'th1'), ('th', 'th2'), ('tr', 'tr0'), ('tr', 'tr1'), ('tr', 'tr2'), ('tl', 'tl0'), ('tl', 'tl1'), ('tl', 'tl2'),
	('tm', 'tm00'), ('tm', 'tm01'), ('tm', 'tm10'), ('tm', 'tm11')) x(t, leaf) \gexec
VACUUM th, tr, tl, tm;
SELECT 'start' AS "case", leaves_differing('th') AS h, leaves_differing('tr') AS r, leaves_differing('tl') AS l, leaves_differing('tm') AS m;

-- an upsert with a BEFORE ROW trigger that reads the table: the rows it replaced are not counted twice
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO th SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i ON CONFLICT (id) DO UPDATE SET v = th.v + 1;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO tr SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i ON CONFLICT (id) DO UPDATE SET v = tr.v + 1;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO tl SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i ON CONFLICT (id, g) DO UPDATE SET v = tl.v + 1;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO tm SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i ON CONFLICT (id) DO UPDATE SET v = tm.v + 1;
SELECT * FROM log_summary();
SELECT 'upsert' AS "case", leaves_differing('th') AS h, leaves_differing('tr') AS r, leaves_differing('tl') AS l, leaves_differing('tm') AS m;
TRUNCATE trlog;
SELECT format('DROP TRIGGER trb ON %s', c) FROM unnest(ARRAY['th0', 'th1', 'th2', 'tr0', 'tr1', 'tr2', 'tl0', 'tl1', 'tl2', 'tm00', 'tm01', 'tm10', 'tm11']) c \gexec

-- a function in SET, WHERE and RETURNING of the upsert reads the table too (after REINDEX: once a statement has ended versions of a table in the log, the next one is read right whatever the library hears)
REINDEX TABLE th;
REINDEX TABLE tr;
VACUUM th, tr;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO th SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i
	ON CONFLICT (id) DO UPDATE SET v = th.v + log_value('th set', 'th') WHERE log_value('th where', 'th') > 0 RETURNING log_value('th returning', 'th') AS returned;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO tr SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i
	ON CONFLICT (id) DO UPDATE SET v = tr.v + log_value('tr set', 'tr') WHERE log_value('tr where', 'tr') > 0 RETURNING log_value('tr returning', 'tr') AS returned;
SELECT * FROM log_summary() WHERE ev NOT LIKE '% BEFORE %';
TRUNCATE trlog;

REINDEX TABLE tm;
REINDEX TABLE tl;
VACUUM tm, tl;
-- a data-modifying CTE that upserts, with a function that reads the table for each row it returns (the statement itself names no table that reads the index: nothing loads the library before it runs)
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
WITH ins AS (INSERT INTO tm SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i ON CONFLICT (id) DO UPDATE SET v = tm.v + 1 RETURNING id)
SELECT count(log_value('tm cte', 'tm')) AS rows_returned FROM ins;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
WITH ins AS (INSERT INTO tl SELECT i * 3, i % 7, i FROM generate_series(2390, 2409) i ON CONFLICT (id, g) DO UPDATE SET v = tl.v + 1 RETURNING id)
SELECT count(log_value('tl cte', 'tl')) AS rows_returned FROM ins;
SELECT * FROM log_summary() WHERE ev NOT LIKE '% BEFORE %';
TRUNCATE trlog;
SELECT 'functions and CTEs' AS "case", leaves_differing('th') AS h, leaves_differing('tr') AS r, leaves_differing('tl') AS l, leaves_differing('tm') AS m;

REINDEX TABLE tr;
VACUUM tr;
-- MERGE as the first statement: update, delete and insert
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
MERGE INTO tr USING (SELECT i * 3 AS id, i % 7 AS g, i AS v FROM generate_series(2380, 2419) i) s ON tr.id = s.id
	WHEN MATCHED AND s.v % 3 = 0 THEN DELETE WHEN MATCHED THEN UPDATE SET v = tr.v + log_value('tr merge update', 'tr')
	WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.g, s.v + log_value('tr merge insert', 'tr'));
SELECT * FROM log_summary();
SELECT 'merge' AS "case", leaves_differing('tr') AS r;
TRUNCATE trlog;

-- COPY, then reads in the same transaction, an update of the copied rows, a savepoint that rolls back an upsert
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
COPY th FROM STDIN;
90001	1	1
90002	2	2
90003	3	3
\.
SELECT read_sift('th') = read_heap('th') AS same_after_copy;
UPDATE th SET v = v + 1 WHERE id > 90000;
SELECT read_sift('th') = read_heap('th') AS same_after_update;
SAVEPOINT s;
INSERT INTO th SELECT i * 3, i % 7, i FROM generate_series(0, 2699) i ON CONFLICT (id) DO UPDATE SET v = th.v + 1;
SELECT read_sift('th') = read_heap('th') AS same_after_upsert;
ROLLBACK TO s;
SELECT read_sift('th') = read_heap('th') AS same_after_rollback;
COMMIT;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
SAVEPOINT s;
INSERT INTO tl SELECT i * 3, i % 7, i FROM generate_series(0, 2699) i ON CONFLICT (id, g) DO UPDATE SET v = tl.v + 1;
SELECT read_sift('tl') = read_heap('tl') AS same_after_upsert;
ROLLBACK TO s;
INSERT INTO tl SELECT i * 3, i % 7, i FROM generate_series(0, 99) i ON CONFLICT (id, g) DO UPDATE SET v = tl.v + 2;
SELECT read_sift('tl') = read_heap('tl') AS same_after_rollback_and_upsert;
COMMIT;
SELECT 'copy and savepoints' AS "case", leaves_differing('th') AS h, leaves_differing('tl') AS l;

-- prepared statements and the extended protocol: the first statement of the backend is a plan that was not made by it
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
PREPARE up(int, int) AS INSERT INTO tm SELECT i * 3, i % 7, i FROM generate_series($1, $2) i ON CONFLICT (id) DO UPDATE SET v = tm.v + 1;
EXECUTE up(0, 2699);
SET plan_cache_mode = force_generic_plan;
EXECUTE up(100, 199);
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO tr SELECT i * 3, i % 7, i FROM generate_series(0, $1::int) i ON CONFLICT (id) DO UPDATE SET v = tr.v + 1 \bind 2699 \g
SELECT 'prepared and extended' AS "case", leaves_differing('tm') AS m, leaves_differing('tr') AS r;

-- a partition detached and attached in the transaction that upserts into it and into the table
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
ALTER TABLE tr DETACH PARTITION tr0;
INSERT INTO tr0 SELECT id, g, v FROM tr0 ON CONFLICT (id) DO UPDATE SET v = tr0.v + 1;
ALTER TABLE tr ATTACH PARTITION tr0 FOR VALUES FROM (0) TO (2400);
INSERT INTO tr SELECT id, g, v FROM tr WHERE id % 4 = 0 ON CONFLICT (id) DO UPDATE SET v = tr.v + 2;
SELECT read_sift('tr') = read_heap('tr') AS same_in_the_transaction;
COMMIT;
SELECT 'detach and attach' AS "case", leaves_differing('tr') AS r;

-- the parent of an inheritance tree: its own rows only, its children keep theirs
CREATE TABLE ti (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
CREATE TABLE ti0 (LIKE ti INCLUDING ALL) INHERITS (ti) WITH (autovacuum_enabled = false);
INSERT INTO ti0 SELECT i * 3, i % 7, i FROM generate_series(0, 1199) i;
INSERT INTO ti SELECT i * 3 + 2, i % 7, i FROM generate_series(0, 599) i;
CREATE INDEX ti_c ON ti USING sift_col (id, g, v);
CREATE INDEX ti0_c ON ti0 USING sift_col (id, g, v);
VACUUM ti, ti0;
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO ti SELECT i * 3 + 2, i % 7, i FROM generate_series(0, 699) i ON CONFLICT (id) DO UPDATE SET v = ti.v + 1;
UPDATE ti SET v = v + 1 WHERE id % 5 = 0;
SELECT 'inheritance' AS "case", sift_helpers.groups_differences_sift_vs_heap('ti') AS differing, sift_helpers.groups_differences_sift_vs_heap('ti0') AS differing_child;

DROP TABLE th, tr, tl, tm, ti0, ti, trlog;
DROP FUNCTION read_sift, read_heap, log_reads, log_trigger, log_value, log_summary, leaves_differing;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
