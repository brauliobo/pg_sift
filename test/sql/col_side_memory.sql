-- The memory of the scope sidecar a build writes (col/seg_write/side.c, scope col_side_build of core/membudget.h) follows maintenance_work_mem, not the rows or the size of the sidecar, and the build
-- writes no temporary file: the rows of the open run take at most five eighths of it (a run is cut when the next unit would pass that, and at 131,072 rows), the bytes of the sidecar wait in a buffer
-- of a quarter of it (side_sink_max of core/limits.h lowers that for a test) and the run records of a scope take at most an eighth (their buffers double: a quarter). A sidecar that fits its buffer is put to pages taken to the page, one
-- that outgrows it is put to the pages as it is made. The checkpoints of the scope fail the statement when the tree of its context passes the budget; here the bytes are read by context kind
-- (sift.test_mem_kinds) at the peak and held to the terms that make them: the rows of the run (a value in each leaf and in the time, the bits and the month of a row: 32 bytes) and its month copies and
-- cube scratch (the work context: 48 bytes a row and the arrays of a cube), the arrays of the largest unit, the buffer, and the run records. A sidecar put to the pages as it is made has the bytes, the
-- cubes and the runs of one written from memory, and the index reads as the join does.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.test_mem_kinds = on;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 100) m;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 100) m WHERE m % 2 = 0;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);

CREATE TABLE peaks (name text, nrows int, mwm_kb int, sink_max int8, unit_rows int, peak int8, budget int8, build int8, work int8, image int8, seg_bytes int8, temp_added int8, cells text, sidecar_runs int);
CREATE TABLE temps (mark text, bytes int8);

-- a: maintenance_work_mem 1 MB, the buffer lowered to 4 kB (the sidecar is put to the pages as it is made); b: 2 MB, the buffer its 512 kB (the sidecar is held and written to the page); b2: 2 MB with
-- the buffer lowered (the same sidecar put as it is made); c: 64 MB, units of 10,000 rows, so that a run is cut at 131,072 rows
CREATE TABLE plan (n serial, name text, mwm_kb int, sink_max int8, unit_rows int, nrows int);
INSERT INTO plan (name, mwm_kb, sink_max, unit_rows, nrows)
	VALUES ('a', 1024, 4096, 1000, 160000), ('a', 1024, 4096, 1000, 320000), ('a', 1024, 4096, 1000, 640000), ('b', 2048, 1125899906842624, 1000, 640000), ('b2', 2048, 4096, 1000, 640000),
		   ('c', 65536, 1125899906842624, 10000, 320000), ('c', 65536, 1125899906842624, 10000, 640000);

CREATE FUNCTION mark(m text) RETURNS text LANGUAGE sql AS
$$ SELECT format('INSERT INTO temps SELECT %L, temp_bytes FROM pg_stat_database WHERE datname = current_database()', m) $$;
CREATE FUNCTION steps(p plan) RETURNS SETOF text LANGUAGE sql AS
$$ SELECT 'TRUNCATE events, temps'
   UNION ALL SELECT format('SET sift.col_unit_rows = %s', p.unit_rows)
   UNION ALL SELECT format('INSERT INTO events SELECT i, 1 + i %% 100, timestamptz ''2025-01-01'' + i * interval ''17 minutes'', i %% 7, (i * 37) %% 1000 FROM generate_series(1, %s) i', p.nrows)
   UNION ALL SELECT 'VACUUM (FREEZE, ANALYZE) events'
   UNION ALL SELECT format('SET maintenance_work_mem = %s', p.mwm_kb)
   UNION ALL SELECT format('SET sift.test_limits = %L', 'side_sink_max=' || p.sink_max)
   UNION ALL SELECT 'DROP INDEX IF EXISTS events_col'
   UNION ALL SELECT 'DO $f$ BEGIN PERFORM pg_stat_force_next_flush(); END $f$'
   UNION ALL SELECT mark('dim_a before')
   UNION ALL SELECT $c$CREATE INDEX events_ent ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at')$c$
   UNION ALL SELECT 'DO $f$ BEGIN PERFORM pg_stat_force_next_flush(); END $f$'
   UNION ALL SELECT mark('dim_a after')
   UNION ALL SELECT 'DROP INDEX events_ent'
   UNION ALL SELECT 'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)'
   UNION ALL SELECT 'DO $f$ BEGIN PERFORM pg_stat_force_next_flush(); END $f$'
   UNION ALL SELECT mark('scoped before')
   UNION ALL SELECT $c$CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')$c$
   UNION ALL SELECT 'DO $f$ BEGIN PERFORM pg_stat_force_next_flush(); END $f$'
   UNION ALL SELECT mark('scoped after')
   UNION ALL SELECT format($i$INSERT INTO peaks SELECT %L, %s, %s, %s, %s, h.peak, h.budget,
		(SELECT bytes FROM sift._test_mem_kinds() WHERE scope = 'col_side_build' AND kind = 'sift col side build'),
		coalesce((SELECT bytes FROM sift._test_mem_kinds() WHERE scope = 'col_side_build' AND kind = 'sift col side work'), 0),
		(SELECT bytes FROM sift._test_mem_kinds() WHERE scope = 'col_side_build' AND kind = 'sift col side image'),
		(SELECT max(bytes) FROM sift.col_segments('events_col') WHERE kind = 'immutable'),
		(SELECT max(bytes) FILTER (WHERE mark = 'scoped after') - max(bytes) FILTER (WHERE mark = 'scoped before') - (max(bytes) FILTER (WHERE mark = 'dim_a after') - max(bytes) FILTER (WHERE mark = 'dim_a before')) FROM temps),
		(SELECT md5(string_agg(concat_ws('/', c.key, c.run, c.units, c.cell, c.grp, c.rows, c.sum), ',' ORDER BY c.key, c.run, c.cell, c.grp)) FROM sift._test_side_cells('events_col', 0, 4, 5) c),
		(SELECT count(*) FROM sift._test_side_valid('events_col', 0, 1))
	FROM sift._test_mem_highwater() h WHERE h.scope = 'col_side_build'$i$, p.name, p.nrows, p.mwm_kb, p.sink_max, p.unit_rows) $$;

SELECT s FROM plan p, LATERAL steps(p) s ORDER BY p.n \gexec

-- the scope ran once for each build and its checkpoints held the tree under the budget (the statement fails past it)
SELECT name, nrows, peak <= budget AS within_budget FROM peaks ORDER BY name, nrows;

-- no temporary file: the build of the index with the scope wrote the bytes of the one without it and no more
SELECT name, nrows, temp_added AS temp_bytes_of_the_sidecar FROM peaks ORDER BY name, nrows;

-- the bytes of the contexts at the peak. The rows of a run and the work of its cubes (the month copies of its rows, the groups and positions of a cube, its calendar days) are sized so that they take five eighths of
-- maintenance_work_mem whatever the leaves the cube reads (side_row_bytes), but one unit at least: that, the arrays of the largest unit (16 bytes a row, at most twice the unit rows), the run records (an eighth of maintenance_work_mem;
-- the buffers double: a quarter) and the arrays of a cube (1 MB) are the terms of the scope's contexts. The image holds the buffer: a quarter of maintenance_work_mem or what the limit leaves of it, to the byte
-- and the page the stream writes from (8 kB) and the lists it keeps
SELECT name, nrows, p.build + p.work <= p.mwm_kb * 1024 / 8 * 5 + p.unit_rows * 32 + p.mwm_kb * 1024 / 4 + 1048576 AS scope_within_its_terms,
	p.image BETWEEN least(p.mwm_kb * 1024 / 4, p.sink_max) AND least(p.mwm_kb * 1024 / 4, p.sink_max) + 24576 AS image_is_the_buffer
FROM peaks p ORDER BY name, nrows;

-- from 160,000 rows to 640,000 (the sidecar is four times larger) the context of the scope grows by at most the run records of the runs it adds
SELECT max(build) FILTER (WHERE nrows = 640000) - max(build) FILTER (WHERE nrows = 160000) <= 16384 AS flat_in_the_rows FROM peaks WHERE name = 'a';

-- a sidecar put to the pages as it is made is the one written from memory: the same bytes, cubes and runs (b: the buffer held it, b2: it was put as it was made)
SELECT a.seg_bytes = b.seg_bytes AS same_bytes, a.cells = b.cells AS same_cubes, a.sidecar_runs = b.sidecar_runs AS same_runs, a.sidecar_runs > 0 AS has_runs, a.image < 65536 AS b2_put_as_made, b.image >= 524288 AS b_held
FROM peaks a, peaks b WHERE a.name = 'b2' AND b.name = 'b';
-- a smaller buffer, a smaller context
SELECT a.build + a.image < b.build + b.image AS smaller_buffer_smaller_context FROM peaks a, peaks b WHERE a.name = 'b2' AND b.name = 'b';

-- a run of the 131,072 rows of the cap is cut by rows (units of 10,000 rows) and by memory (units of 1,000 rows at 1 MB): the key reads the runs and the answer is the join's
SELECT sift.col_verify('events_col', true);
SELECT count(*) AS scope_problems FROM sift.col_scope_verify('events_col', 100);

-- several scopes: one scope is in memory at a time (the bytes of a finished scope are in the sidecar's buffer or its pages), so the memory of the build is that of the largest scope, not of their number. One, two
-- and four scopes of the same pairs on one index (maintenance_work_mem 1 MB, the buffer lowered): the build stays within its budget, the context of the scopes at the peak is that of one, and each scope has the
-- runs and the cubes of the first while the sidecar is four times larger
CREATE TABLE multi_peaks (n int, peak int8, budget int8, build int8, work int8, seg_bytes int8, cells text, same_cells boolean, same_runs boolean, runs int);
CREATE FUNCTION multi_case(n int) RETURNS void LANGUAGE plpgsql AS
$$ DECLARE decl text := 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)';
BEGIN
	FOR i IN 2..n LOOP
		decl := decl || format('; tenant%s_id: dim_a_id -> tenant_scope%s(dim_a_id, tenant%s_id)', i, i, i);
	END LOOP;
	SET maintenance_work_mem = 1024;
	SET sift.test_limits = 'side_sink_max=4096';
	PERFORM count(*) * 0 FROM sift._test_mem_highwater(true);
	DROP INDEX IF EXISTS events_col;
	EXECUTE format($c$CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
		WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = %L)$c$, decl);
	INSERT INTO multi_peaks SELECT n, h.peak, h.budget, (SELECT bytes FROM sift._test_mem_kinds() WHERE scope = 'col_side_build' AND kind = 'sift col side build'),
		(SELECT bytes FROM sift._test_mem_kinds() WHERE scope = 'col_side_build' AND kind = 'sift col side work'),
		(SELECT max(bytes) FROM sift.col_segments('events_col') WHERE kind = 'immutable'),
		(SELECT md5(string_agg(concat_ws('/', c.key, c.run, c.units, c.cell, c.grp, c.rows, c.sum), ',' ORDER BY c.key, c.run, c.cell, c.grp)) FROM sift._test_side_cells('events_col', 0, 4, 5) c),
		(SELECT md5(string_agg(concat_ws('/', c.key, c.run, c.units, c.cell, c.grp, c.rows, c.sum), ',' ORDER BY c.key, c.run, c.cell, c.grp)) FROM sift._test_side_cells('events_col', 0, 4, 5) c)
			= (SELECT md5(string_agg(concat_ws('/', c.key, c.run, c.units, c.cell, c.grp, c.rows, c.sum), ',' ORDER BY c.key, c.run, c.cell, c.grp)) FROM sift._test_side_cells('events_col', n - 1, 4, 5) c),
		(SELECT count(*) FROM sift._test_side_valid('events_col', 0, 1)) = (SELECT count(*) FROM sift._test_side_valid('events_col', n - 1, 1)),
		(SELECT count(*) FROM sift._test_side_valid('events_col', n - 1, 1))
	FROM sift._test_mem_highwater() h WHERE h.scope = 'col_side_build';
END $$;
CREATE TABLE tenant_scope2 (LIKE tenant_scope INCLUDING ALL);
CREATE TABLE tenant_scope3 (LIKE tenant_scope INCLUDING ALL);
CREATE TABLE tenant_scope4 (LIKE tenant_scope INCLUDING ALL);
INSERT INTO tenant_scope2 SELECT * FROM tenant_scope;
INSERT INTO tenant_scope3 SELECT * FROM tenant_scope;
INSERT INTO tenant_scope4 SELECT * FROM tenant_scope;
ALTER TABLE tenant_scope2 RENAME COLUMN tenant_id TO tenant2_id;
ALTER TABLE tenant_scope3 RENAME COLUMN tenant_id TO tenant3_id;
ALTER TABLE tenant_scope4 RENAME COLUMN tenant_id TO tenant4_id;
CREATE INDEX tenant_scope2_journal ON tenant_scope2 USING sift_col (dim_a_id, tenant2_id) WITH (track_changes = 'ctid');
CREATE INDEX tenant_scope3_journal ON tenant_scope3 USING sift_col (dim_a_id, tenant3_id) WITH (track_changes = 'ctid');
CREATE INDEX tenant_scope4_journal ON tenant_scope4 USING sift_col (dim_a_id, tenant4_id) WITH (track_changes = 'ctid');
TRUNCATE events;
SET sift.col_unit_rows = 1000;
INSERT INTO events SELECT i, 1 + i % 100, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 640000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope2, tenant_scope3, tenant_scope4;
SELECT multi_case(1);
SELECT multi_case(2);
SELECT multi_case(4);
RESET sift.test_limits;
SELECT n, peak <= budget AS within_budget, same_cells, same_runs, runs > 0 AS has_runs FROM multi_peaks ORDER BY n;
SELECT (SELECT max(build + work) FROM multi_peaks) <= 1024 * 1024 / 8 * 5 + 1000 * 32 + 1024 * 1024 / 4 + 1048576 AS scopes_within_the_terms_of_one,
	(SELECT seg_bytes FROM multi_peaks WHERE n = 4) > 3 * (SELECT seg_bytes FROM multi_peaks WHERE n = 1) AS sidecar_grows_with_the_scopes;
RESET maintenance_work_mem;
SELECT sift.col_verify('events_col', true);
DROP FUNCTION multi_case(int);
DROP INDEX events_col;
DROP TABLE multi_peaks, tenant_scope2, tenant_scope3, tenant_scope4;

-- a text member adds the filters of its runs: they are put to the sidecar as the others are, and the sidecar put as it is made is the one written from memory (the same bytes, cubes and runs), and the reads are the join's
CREATE TABLE text_pairs (account text NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO text_pairs SELECT 'acct-' || m, 1 FROM generate_series(1, 100) m;
INSERT INTO text_pairs SELECT 'acct-' || m, 2 FROM generate_series(1, 100) m WHERE m % 2 = 0;
CREATE INDEX text_pairs_journal ON text_pairs USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE text_events (id int PRIMARY KEY, account text NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
SET sift.col_unit_rows = 1000;
INSERT INTO text_events SELECT i, 'acct-' || (1 + i % 100), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 320000) i;
VACUUM (FREEZE, ANALYZE) text_events;
CREATE TABLE text_peaks (name text, seg_bytes int8, cells text, sidecar_runs int, answers text);
CREATE FUNCTION text_case(name text, mwm_kb int, sink_max int8) RETURNS void LANGUAGE plpgsql AS
$$ BEGIN
	EXECUTE format('SET maintenance_work_mem = %s', mwm_kb);
	EXECUTE format('SET sift.test_limits = %L', 'side_sink_max=' || sink_max);
	DROP INDEX IF EXISTS text_events_col;
	CREATE INDEX text_events_col ON text_events USING sift_col (id, account, created_at, kind, v) WITH (key_cut = 'created_at', scope = 'tenant_id: account -> text_pairs(account, tenant_id)');
	INSERT INTO text_peaks SELECT name, (SELECT max(bytes) FROM sift.col_segments('text_events_col') WHERE kind = 'immutable'),
		(SELECT md5(string_agg(concat_ws('/', c.key, c.run, c.units, c.cell, c.grp, c.rows, c.sum), ',' ORDER BY c.key, c.run, c.cell, c.grp)) FROM sift._test_side_cells('text_events_col', 0, 4, 5) c),
		(SELECT count(*) FROM sift._test_side_valid('text_events_col', 0, 1)),
		(SELECT string_agg(r.kind || ':' || r.n || ':' || r.s, ',' ORDER BY r.kind) FROM
			(SELECT kind, count(*) n, sum(v) s FROM text_events e WHERE sift.in_scope('text_events_col'::regclass, 'tenant_id', e.account, 1) GROUP BY kind) r);
END $$;
SET sift.agg_force = on;
SET sift.test_scope_entity = on;
SELECT text_case('put as made', 1024, 4096);
SELECT text_case('held', 1024, 1125899906842624);
RESET sift.test_limits;
SELECT a.seg_bytes = b.seg_bytes AS same_bytes, a.cells = b.cells AS same_cubes, a.sidecar_runs = b.sidecar_runs AS same_runs, a.sidecar_runs > 0 AS has_runs, a.answers = b.answers AS same_answers
FROM text_peaks a, text_peaks b WHERE a.name = 'put as made' AND b.name = 'held';
SET sift.enable_agg = off;
SELECT string_agg(r.kind || ':' || r.n || ':' || r.s, ',' ORDER BY r.kind) = (SELECT answers FROM text_peaks WHERE name = 'held') AS answers_are_the_joins
FROM (SELECT kind, count(*) n, sum(v) s FROM text_events e WHERE e.account IN (SELECT account FROM text_pairs WHERE tenant_id = 1) GROUP BY kind) r;
RESET sift.enable_agg;
SELECT sift.col_verify('text_events_col', true);

-- the run records of a scope are held at most an eighth of maintenance_work_mem (96 kB at 768 kB: a run takes its record of 56 bytes and, with a key of its own, 16 more): 1,500 keys of two members each, 1,500
-- units apart, have 1,500 runs of two units, and the scope keeps the runs of its first keys; the keys after them have no run and are read by reference, which gives the rows of the join
CREATE TABLE wide_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO wide_scope SELECT k, k FROM generate_series(1, 1500) k;
INSERT INTO wide_scope SELECT k + 1500, k FROM generate_series(1, 1500) k;
CREATE INDEX wide_scope_journal ON wide_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE wide_events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
SET sift.col_unit_rows = 150;
INSERT INTO wide_events SELECT i, 1 + i % 3000, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) wide_events, wide_scope;
SET maintenance_work_mem = 768;
CREATE INDEX wide_col ON wide_events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> wide_scope(dim_a_id, tenant_id)');
RESET maintenance_work_mem;
SELECT count(DISTINCT key) BETWEEN 1000 AND 1365 AS keys_with_runs_up_to_the_tables_budget FROM sift._test_side_cells('wide_col', 0, 4, 5);
SELECT count(*) AS scope_problems FROM sift.col_scope_verify('wide_col', 300);
SELECT sift.col_verify('wide_col', true);
DROP TABLE wide_events, wide_scope;

DROP FUNCTION mark(text);
DROP FUNCTION steps(plan), text_case(text, int, int8);
DROP TABLE plan, peaks, temps, text_peaks, events, tenant_scope, text_events, text_pairs;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
