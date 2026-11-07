-- The tables of the plan-choice gate (scripts/plangate.sh, test/plan/shapes.sql): events (the fact rows, :n of them), dims (the parents, one per
-- 500 events, at least 60), facts (a second, smaller fact table), a month spine, spans and a partitioned copy of the events. Every draw is a
-- hash of (row number, seed): the same arguments give the same rows, and ANALYZE reads every row (statistics target 1000) and VACUUM marks every page visible, so the planner sees
-- the same statistics on every run.
--
--   psql -v n=30000 -v seed=1 -f gen.sql
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 30000
\endif
\if :{?seed}
\else
\set seed 1
\endif
SET default_statistics_target = 1000;
-- an asynchronous commit leaves the hint bit of the inserted rows unset until the WAL writer flushes, and VACUUM marks a page visible only when all its rows have it
SET synchronous_commit = on;
SELECT greatest(60, :n / 500) AS np \gset

CREATE TABLE dims (id int PRIMARY KEY, tier text NOT NULL, active boolean NOT NULL, settings jsonb NOT NULL);
INSERT INTO dims SELECT p, CASE WHEN p % 3 = 0 THEN 'tier_a' WHEN p % 3 = 1 THEN 'tier_b' ELSE 'tier_c' END, p % 7 <> 0,
	jsonb_build_object('tier', CASE WHEN p <= 2 THEN 'tier_a' WHEN p <= :np / 2 THEN 'tier_b' ELSE 'tier_c' END, 'slot', p % 3) FROM generate_series(1, :np) p;

-- the parents of the events by skew_id, read as a lookup through a column index of their own
CREATE TABLE parents (id int PRIMARY KEY, name text NOT NULL);
INSERT INTO parents SELECT p, 'parent ' || p FROM generate_series(1, :np) p;
CREATE INDEX parents_col ON parents USING sift_col (id, name) WITH (key_columns = 'auto: id', order_array_columns = 'auto: id');

CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, skew_id int NOT NULL, ts date NOT NULL, kind text NOT NULL, amount float8 NOT NULL, qty int NOT NULL,
	deleted_at timestamptz, doc jsonb NOT NULL, memo text NOT NULL) WITH (autovacuum_enabled = false);
-- parent_id is flat; skew_id is log-uniform (parent 1 owns about one row in six); amount is a multiple of 0.25 (sums are exact), the metric m_a one decimal (sums round)
INSERT INTO events SELECT i, 1 + (i::bigint * 7919) % :np, floor(power(:np + 1.0, (i * 0.6180339887498949) % 1.0))::int,
	date '2024-01-01' + (hashint4(i + :seed) & 1023) % 730, 'cat_' || chr(97 + i % 4), 1 + (i % 17) + 0.25 * (i % 4), 1 + i % 9, CASE WHEN i % 37 = 0 THEN now() END,
	jsonb_build_object(
		'parts', jsonb_build_array(
			jsonb_build_object('kind', 'cat_' || chr(97 + i % 4), 'sub', 'catg_' || chr(97 + i % 6), 'dir_a', CASE WHEN i % 3 = 0 THEN 'neg' ELSE 'pos' END, 'score', 1 + (i * 3) % 10),
			jsonb_build_object('kind', 'cat_' || chr(97 + (i + 1) % 4), 'sub', 'catg_' || chr(97 + (i + 2) % 6), 'dir_a', CASE WHEN i % 5 = 0 THEN 'neg' ELSE 'pos' END, 'score', 1 + (i * 7) % 10)),
		'metrics', jsonb_build_object('m_a', ((i * 13) % 100) / 10.0, 'catg_a', 1 + (i * 3) % 10, 'catg_b', 1 + (i * 5) % 10)),
	(ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + i % 8] || ' ' || (ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + (i * 3 + 1) % 8]
		|| ' ' || (ARRAY['relay', 'cord', 'clamp', 'bolt', 'valve'])[1 + (i * 7) % 5]
	FROM generate_series(1, :n) i;
CREATE INDEX events_parent ON events (parent_id, ts, deleted_at);
CREATE INDEX events_skew ON events (skew_id, ts, deleted_at);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, skew_id, ts, kind, amount, qty, deleted_at, doc)
	WITH (paths = 'doc.metrics.* float8, doc.parts[]{kind astext, sub astext, dir_a astext, score float8}');
CREATE INDEX events_memo ON events USING sift (memo);

CREATE TABLE facts (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, role text NOT NULL, val float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + (i::bigint * 104729) % :np, date '2024-01-01' + (hashint4(i + :seed + 7) & 1023) % 730, 'type_' || chr(97 + i % 3), 1 + (i % 11) + 0.5 * (i % 2)
	FROM generate_series(1, :n / 3) i;
CREATE INDEX facts_parent ON facts (parent_id, ts);
CREATE INDEX facts_col ON facts USING sift_col (id, parent_id, ts, role, val);

-- pings: the latest row of each parent (a sub-select that is the first row of its parent in the order of ts): the order array of ts is grouped by parent_id; ts is unique, status passes one row in 400
CREATE TABLE pings (id int PRIMARY KEY, parent_id int NOT NULL, ts timestamp NOT NULL, status text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO pings SELECT i, 1 + (i::bigint * 104729) % :np, timestamp '2024-01-01' + ((i::bigint * 7919) % 100003) * interval '5 minutes', 'st_' || lpad((i % 400)::text, 4, '0')
	FROM generate_series(1, :n / 3) i;
CREATE INDEX pings_parent ON pings (parent_id, ts);
CREATE INDEX pings_col ON pings USING sift_col (id, parent_id, ts, status) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'parent_id');

CREATE TABLE buckets (bucket date PRIMARY KEY, span text NOT NULL);
INSERT INTO buckets SELECT d::date, CASE WHEN d < date '2025-01-01' THEN 'span_b' ELSE 'span_a' END FROM generate_series(date '2024-01-01', date '2025-12-01', interval '1 month') d;

-- the events again, in four partitions by the half year of ts
CREATE TABLE events_part (id int NOT NULL, parent_id int NOT NULL, ts date NOT NULL, kind text NOT NULL, amount float8 NOT NULL, deleted_at timestamptz) PARTITION BY RANGE (ts);
CREATE TABLE events_part_1 PARTITION OF events_part FOR VALUES FROM ('2024-01-01') TO ('2024-07-01') WITH (autovacuum_enabled = false);
CREATE TABLE events_part_2 PARTITION OF events_part FOR VALUES FROM ('2024-07-01') TO ('2025-01-01') WITH (autovacuum_enabled = false);
CREATE TABLE events_part_3 PARTITION OF events_part FOR VALUES FROM ('2025-01-01') TO ('2025-07-01') WITH (autovacuum_enabled = false);
CREATE TABLE events_part_4 PARTITION OF events_part FOR VALUES FROM ('2025-07-01') TO ('2026-01-01') WITH (autovacuum_enabled = false);
INSERT INTO events_part SELECT id, parent_id, ts, kind, amount, deleted_at FROM events;
SELECT format('CREATE INDEX %1$s_parent ON %1$s (parent_id, ts, deleted_at); CREATE INDEX %1$s_col ON %1$s USING sift_col (id, parent_id, ts, kind, amount, deleted_at)', c.relname) AS ddl
	FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid ORDER BY c.relname \gexec

-- the visible fraction of a table prices the index-only scans of the native plan and so every cost ratio of the gate, and a first pass has been seen to set the all-visible bit of only
-- part of the pages it freezes (a different part each time): a table left with pages that are not all visible gets a second pass, and one that still has them stops the run
\set unseen 'SELECT count(*) > 0 AS partial, coalesce(string_agg(relname || '' '' || relallvisible || ''/'' || relpages, '', ''), '''') AS tables FROM pg_class WHERE relnamespace = ''public''::regnamespace AND relkind = ''r'' AND relallvisible < relpages'
VACUUM (FREEZE, ANALYZE) dims, parents, events, facts, pings, buckets, events_part;
:unseen \gset
\if :partial
\echo gen.sql: pages not all visible after the first VACUUM (allvisible/pages): :tables
VACUUM (FREEZE, ANALYZE) dims, parents, events, facts, pings, buckets, events_part;
:unseen \gset
\endif
\if :partial
\echo gen.sql: pages not all visible after the second VACUUM: :tables
DO $$ BEGIN RAISE EXCEPTION 'gen.sql: a table has pages that are not all visible after two VACUUMs (a transaction holding the horizon?)'; END $$;
\endif

-- a copy of the events whose heap pages are all dirty after its VACUUM (the first row of every page updated: the update clears the page's bit of the visibility map, and ANALYZE
-- counts the pages again for the index-only scans of the native plan): the rows are checked in the heap, page by page, and the scan is priced for it (vet_cost)
CREATE TABLE events_dirty (id int PRIMARY KEY, set_id int NOT NULL, kind text NOT NULL, ts date NOT NULL, amount float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events_dirty SELECT id, 1 + id % 8, kind, ts, amount FROM events;
CREATE INDEX events_dirty_set ON events_dirty (set_id, ts) INCLUDE (kind, amount);
CREATE INDEX events_dirty_col ON events_dirty USING sift_col (set_id, kind, ts, amount);
VACUUM (FREEZE, ANALYZE) events_dirty;
UPDATE events_dirty SET amount = amount WHERE (ctid::text::point)[1]::int = 1;
ANALYZE events_dirty;

-- numeric amounts (a distinct value for about three rows) and rates (forty values, as a price list) with two lookups of their own, for the aggregates over an expression of a column of the table and a column of a lookup:
-- PostgreSQL evaluates it once per row (a numeric product), the scan once per distinct pair of values
CREATE TABLE dim_b (id int PRIMARY KEY, set_id int NOT NULL);
INSERT INTO dim_b SELECT k, 1 + k % 4 FROM generate_series(1, 30) k;
CREATE TABLE dim_c (id int PRIMARY KEY, category_id int NOT NULL);
INSERT INTO dim_c SELECT o, 1 + o % 6 FROM generate_series(1, greatest(60, :n / 15)) o;
CREATE TABLE facts_b (id int PRIMARY KEY, dim_a_id int NOT NULL, foreign_id int NOT NULL, status text NOT NULL, amount numeric(12,2) NOT NULL, qty int NOT NULL, rate numeric(12,2) NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts_b SELECT i, 1 + (hashint4(i + :seed) & 2147483647) % greatest(60, :n / 15), 1 + (hashint4(i * 3 + :seed) & 2147483647) % 30,
	(ARRAY['open', 'paid', 'void', 'late'])[1 + i % 4], ((hashint4(i * 7 + :seed) & 2147483647) % greatest(100, :n / 3)) / 10.0, 1 + i % 50, (1 + i % 40) / 4.0 FROM generate_series(1, :n) i;
CREATE INDEX facts_b_dim_a ON facts_b (dim_a_id);
CREATE INDEX facts_b_col ON facts_b USING sift_col (id, dim_a_id, foreign_id, status, amount, qty, rate);
CREATE TABLE links_b (id int PRIMARY KEY, parent_id int NOT NULL, label text NOT NULL, weight int NOT NULL, status smallint NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO links_b SELECT i, 1 + (hashint4(i + :seed) & 2147483647) % greatest(60, :n / 15), 'lbl_' || i % 10, i / 7 % 3, i / 3 % 5 FROM generate_series(1, 4 * :n) i;
CREATE INDEX links_b_parent ON links_b (parent_id, status);
VACUUM (FREEZE, ANALYZE) facts_b, dim_b, dim_c, links_b;
