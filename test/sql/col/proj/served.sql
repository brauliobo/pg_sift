-- A projection that an existing sift_col index of the table already holds (its columns and expressions, its predicate implied by the projection's filter, its rollup and
-- options, the same layout) is served by that index: sift.projection_index builds nothing and the index lists the name. The planner reads the index for the queries of the
-- projection (the plan names it) and the rows are those of the heap path. An index that stops holding a projection, or goes, makes it broken in the status; the queries
-- then take the normal plan, with the same rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind int, qty int, status text, created_at timestamptz);
INSERT INTO events SELECT i, i % 40, i % 5, i % 13, CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['new', 'open', 'held', 'closed'])[1 + i % 4] END,
  timestamptz '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 6000) i;

-- Which index the plan of a statement reads, and whether its rows are those of the heap path
CREATE FUNCTION answered(q text) RETURNS TABLE (read_by text, differences bigint) LANGUAGE sql AS $$ SELECT sift_helpers.agg_index_used(q), sift_helpers.differences_agg(q) $$;

-- 1. what makes an index serve: a table of cases. Each builds one index with the given columns, options and predicate, defines a projection, asks which index projection_index uses
-- and drops both. served: the index itself; built: a new index.
CREATE FUNCTION served_case(wide_cols text, wide_with text, wide_where text, cols text[], rollups text, filter text, options jsonb, layout jsonb) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('CREATE INDEX wide ON events USING sift_col (%s) WITH (track_changes = off%s) %s', wide_cols, COALESCE(', ' || wide_with, ''), COALESCE('WHERE ' || wide_where, ''));
	PERFORM sift.projection_create('p', 'events', cols, rollups, filter, options);
	r := CASE WHEN sift.projection_index('p_index', ARRAY['p'], layout) = 'p_index' THEN 'built' ELSE 'served' END;
	DROP INDEX IF EXISTS p_index;
	DROP INDEX wide;
	PERFORM sift.projection_drop('p');
	RETURN r;
END $$;
SELECT c.label, c.expect, served_case(c.wide_cols, c.wide_with, c.wide_where, c.cols, c.rollups, c.filter, c.options, c.layout) AS got FROM (VALUES
  ('same columns',            'served', 'dim_a_id, kind, qty',                    NULL, NULL, ARRAY['dim_a_id', 'kind', 'qty'], NULL, NULL, '{}'::jsonb, '{}'::jsonb),
  ('subset',                  'served', 'dim_a_id, kind, qty, status',            NULL, NULL, ARRAY['kind', 'dim_a_id'],        NULL, NULL, '{}', '{}'),
  ('missing column',          'built',  'dim_a_id, kind',                         NULL, NULL, ARRAY['kind', 'qty'],             NULL, NULL, '{}', '{}'),
  ('same expression',         'served', 'dim_a_id, (qty / 10), kind',             NULL, NULL, ARRAY['(qty / 10)', 'kind'],      NULL, NULL, '{}', '{}'),
  ('other expression',        'built',  'dim_a_id, (qty / 10), kind',             NULL, NULL, ARRAY['(qty / 5)', 'kind'],       NULL, NULL, '{}', '{}'),
  ('column for an expression', 'built', 'dim_a_id, qty, kind',                    NULL, NULL, ARRAY['(qty / 10)'],              NULL, NULL, '{}', '{}'),
  ('filter on a full index',  'served', 'dim_a_id, kind, status',                 NULL, NULL, ARRAY['dim_a_id', 'kind'],        NULL, $$status = 'open'$$, '{}', '{}'),
  ('filter equal',            'served', 'dim_a_id, kind',                         NULL, $$status = 'open'$$, ARRAY['dim_a_id'], NULL, $$status = 'open'$$, '{}', '{}'),
  ('filter implies, no column', 'built', 'dim_a_id, kind',                       NULL, $$status <> 'closed'$$, ARRAY['dim_a_id'], NULL, $$status = 'open'$$, '{}', '{}'),
  ('filter implies, column',  'served', 'dim_a_id, kind, status',                 NULL, $$status <> 'closed'$$, ARRAY['dim_a_id'], NULL, $$status = 'open'$$, '{}', '{}'),
  ('filter on an expression', 'built',  'dim_a_id, kind, qty',                    NULL, NULL, ARRAY['dim_a_id'],                NULL, $$qty / 2 = 3$$, '{}', '{}'),
  ('filter narrower index',   'built',  'dim_a_id, kind',                         NULL, $$status = 'open'$$, ARRAY['dim_a_id'], NULL, NULL, '{}', '{}'),
  ('filter weaker',           'built',  'dim_a_id, kind',                         NULL, $$status = 'open'$$, ARRAY['dim_a_id'], NULL, $$status <> 'closed'$$, '{}', '{}'),
  ('filter on a conjunct',    'served', 'dim_a_id, kind',                         NULL, $$kind < 3$$, ARRAY['dim_a_id'],        NULL, $$kind < 2 AND dim_a_id > 5$$, '{}', '{}'),
  ('filter on a disjunction', 'built',  'dim_a_id, kind',                         NULL, $$kind < 3$$, ARRAY['dim_a_id'],        NULL, $$kind < 2 OR dim_a_id > 5$$, '{}', '{}'),
  ('rollup equal',            'served', 'dim_a_id, kind, qty',                    $$rollup_columns = 'dim_a_id, kind'$$, NULL, ARRAY['dim_a_id', 'kind'], 'dim_a_id, kind', NULL, '{}', '{}'),
  ('rollup order',            'served', 'dim_a_id, kind, qty',                    $$rollup_columns = 'dim_a_id, kind'$$, NULL, ARRAY['dim_a_id', 'kind'], 'kind, dim_a_id', NULL, '{}', '{}'),
  ('rollup measures subset',  'served', 'dim_a_id, kind, qty, id',                $$rollup_columns = 'dim_a_id, kind: qty, id'$$, NULL, ARRAY['dim_a_id', 'kind', 'qty'], 'dim_a_id, kind: qty', NULL, '{}', '{}'),
  ('rollup measure missing',  'built',  'dim_a_id, kind, qty, id',                $$rollup_columns = 'dim_a_id, kind: qty'$$, NULL, ARRAY['dim_a_id', 'kind', 'qty', 'id'], 'dim_a_id, kind: qty, id', NULL, '{}', '{}'),
  ('rollup other dims',       'built',  'dim_a_id, kind, qty',                    $$rollup_columns = 'dim_a_id, kind'$$, NULL, ARRAY['dim_a_id', 'kind', 'qty'], 'dim_a_id, qty', NULL, '{}', '{}'),
  ('rollup none in index',    'built',  'dim_a_id, kind',                         $$rollup_columns = 'off'$$, NULL, ARRAY['dim_a_id', 'kind'], 'dim_a_id, kind', NULL, '{}', '{}'),
  ('option equal',            'served', 'dim_a_id, kind',                         'superunit = 8', NULL, ARRAY['dim_a_id'],     NULL, NULL, '{"superunit": 8}', '{}'),
  ('option other',            'built',  'dim_a_id, kind',                         'superunit = 4', NULL, ARRAY['dim_a_id'],     NULL, NULL, '{"superunit": 8}', '{}'),
  ('option missing',          'built',  'dim_a_id, kind',                         NULL, NULL, ARRAY['dim_a_id'],                NULL, NULL, '{"superunit": 8}', '{}'),
  ('layout equal',            'served', 'dim_a_id, kind, qty',                    $$key_order = 'dim_a_id'$$, NULL, ARRAY['kind', 'dim_a_id'], NULL, NULL, '{}', '{"key_order": "dim_a_id"}'),
  ('layout other key',        'built',  'dim_a_id, kind, qty',                    $$key_order = 'dim_a_id'$$, NULL, ARRAY['kind', 'dim_a_id'], NULL, NULL, '{}', '{"key_order": "kind"}'),
  ('layout asked, none held', 'built',  'dim_a_id, kind, qty',                    $$key_cut = 'off', key_order = 'off'$$, NULL, ARRAY['kind', 'dim_a_id'], NULL, NULL, '{}', '{"key_order": "dim_a_id"}'),
  ('layout not asked',        'built',  'dim_a_id, kind, qty',                    $$key_order = 'dim_a_id'$$, NULL, ARRAY['kind', 'dim_a_id'], NULL, NULL, '{}', '{}')
) c(label, expect, wide_cols, wide_with, wide_where, cols, rollups, filter, options, layout);

-- 2. the plan: one wide index with the projection of the widest read, the narrower ones served; each class of query reads that index and returns the rows of the heap path
SELECT sift.projection_create('wide', 'events', ARRAY['dim_a_id', 'created_at', 'kind', 'qty', '(qty / 10)', 'status'], rollups => 'dim_a_id, kind: qty');
SELECT sift.projection_create('by_kind', 'events', ARRAY['dim_a_id', 'kind']);
SELECT sift.projection_create('by_bucket', 'events', ARRAY['dim_a_id', '(qty / 10)']);
SELECT sift.projection_create('by_kind_qty', 'events', ARRAY['dim_a_id', 'kind', 'qty'], rollups => 'dim_a_id, kind: qty');
SELECT sift.projection_index('events_wide', ARRAY['wide'], '{"key_order": "dim_a_id, created_at", "track_changes": "off"}');
SELECT p, sift.projection_index('events_' || p, ARRAY[p], '{"key_order": "dim_a_id, created_at"}') AS index FROM unnest(ARRAY['by_kind', 'by_bucket', 'by_kind_qty']) p;
VACUUM (FREEZE, ANALYZE) events;
SELECT count(*) AS sift_indexes FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col';
SELECT projection, index, state, shared FROM sift.projection_status ORDER BY projection;
CREATE TABLE classes (label text, stmt text);
INSERT INTO classes VALUES
  ('by_kind',     'SELECT kind, count(*) FROM events WHERE dim_a_id < 20 GROUP BY kind'),
  ('by_kind key', 'SELECT kind, count(*) FROM events WHERE dim_a_id = 7 GROUP BY kind'),
  ('by_bucket',   'SELECT qty / 10, count(*) FROM events WHERE dim_a_id < 20 GROUP BY 1'),
  ('by_kind_qty', 'SELECT kind, sum(qty), count(*) FROM events WHERE dim_a_id < 30 GROUP BY kind'),
  ('wide',        'SELECT status, qty / 10, count(*) FROM events WHERE dim_a_id < 30 GROUP BY 1, 2'),
  ('wide window', 'SELECT kind, count(*) FROM events WHERE dim_a_id = 7 AND created_at >= timestamptz ''2026-01-02'' GROUP BY kind');
SELECT c.label, a.read_by, a.differences FROM classes c, LATERAL answered(c.stmt) a ORDER BY c.label;

-- a filtered projection served by a partial index whose predicate its filter implies: the plan reads it for the queries that imply the filter
CREATE TABLE tickets AS TABLE events;
ALTER TABLE tickets ADD PRIMARY KEY (id);
SELECT sift.projection_create('tickets_live', 'tickets', ARRAY['dim_a_id', 'kind', 'qty', 'status'], filter => $$status <> 'closed'$$);
SELECT sift.projection_create('tickets_open', 'tickets', ARRAY['dim_a_id', 'kind', 'status'], filter => $$status = 'open'$$);
SELECT sift.projection_create('tickets_open_qty', 'tickets', ARRAY['dim_a_id', 'kind', 'qty', 'status'], filter => $$status = 'open' AND qty < 9$$);
SELECT sift.projection_create('tickets_exact', 'tickets', ARRAY['dim_a_id', 'kind'], filter => $$kind = 2$$);
SELECT sift.projection_create('tickets_all', 'tickets', ARRAY['dim_a_id', 'kind']);
SELECT sift.projection_index('tickets_live_index', ARRAY['tickets_live'], '{"track_changes": "off"}');
SELECT p, sift.projection_index('tickets_' || p || '_index', ARRAY['tickets_' || p]) AS index FROM unnest(ARRAY['open', 'open_qty', 'exact', 'all']) p;
VACUUM (FREEZE, ANALYZE) tickets;
SELECT projection, index, state, shared FROM sift.projection_status WHERE projection LIKE 'tickets%' ORDER BY projection;
CREATE TABLE ticket_classes (label text, stmt text);
INSERT INTO ticket_classes VALUES
  ('open',           'SELECT kind, count(*) FROM tickets WHERE status = ''open'' AND dim_a_id < 20 GROUP BY kind'),
  ('open and qty',   'SELECT kind, sum(qty) FROM tickets WHERE status = ''open'' AND qty < 9 AND dim_a_id < 20 GROUP BY kind'),
  ('live',           'SELECT kind, count(*) FROM tickets WHERE status <> ''closed'' AND dim_a_id < 20 GROUP BY kind'),
  ('not implied',    'SELECT kind, count(*) FROM tickets WHERE dim_a_id < 20 GROUP BY kind'),
  ('exact',          'SELECT dim_a_id, count(*) FROM tickets WHERE kind = 2 AND dim_a_id < 20 GROUP BY dim_a_id'),
  ('closed',         'SELECT kind, count(*) FROM tickets WHERE status = ''closed'' GROUP BY kind');
SELECT c.label, a.read_by, a.differences FROM ticket_classes c, LATERAL answered(c.stmt) a ORDER BY c.label;
-- every row changes: moves between the statuses, new rows, deleted rows; the rows stay those of the heap path
UPDATE tickets SET status = 'open' WHERE id % 10 = 1;
UPDATE tickets SET status = 'closed' WHERE id % 10 = 2 AND status = 'open';
UPDATE tickets SET status = NULL WHERE id % 10 = 3;
INSERT INTO tickets SELECT 6000 + id, dim_a_id, kind, qty, 'open', created_at FROM events WHERE id % 7 = 0;
DELETE FROM tickets WHERE id % 13 = 0;
SELECT c.label, a.differences FROM ticket_classes c, LATERAL answered(c.stmt) a ORDER BY c.label;

-- 3. the soundness of the filter test: for pairs of random predicates (an index's and a projection's filter), whenever the index is said to serve the filter no row of the table
-- satisfies the filter and fails the predicate (rows with a null status and the three-valued comparisons included); the pairs are fixed by the seed
CREATE TABLE atoms (n serial, atom text);
INSERT INTO atoms (atom) VALUES ('status = ''open'''), ('status <> ''closed'''), ('status IN (''open'', ''new'')'), ('status IS NOT NULL'), ('status IS NULL'), ('kind < 3'), ('kind = 2'),
  ('kind >= 1'), ('qty > 6'), ('qty BETWEEN 2 AND 9'), ('dim_a_id < 20'), ('dim_a_id % 2 = 0'), ('NOT (kind = 4)'), ('id > 1000');
SELECT setseed(0.42);
CREATE TABLE preds AS SELECT g AS n, string_agg(a.atom, CASE WHEN g % 3 = 0 THEN ' OR ' ELSE ' AND ' END) AS pred
FROM generate_series(1, 36) g, LATERAL (SELECT atom FROM (SELECT atom, row_number() OVER (ORDER BY random() + g * 0) rn FROM atoms) r WHERE rn <= 1 + g % 3) a GROUP BY g;
CREATE TABLE filters AS SELECT g AS n, string_agg(a.atom, CASE WHEN g % 4 = 0 THEN ' OR ' ELSE ' AND ' END) AS filter
FROM generate_series(1, 60) g, LATERAL (SELECT atom FROM (SELECT atom, row_number() OVER (ORDER BY random() + g * 0) rn FROM atoms) r WHERE rn <= 1 + g % 3) a GROUP BY g;
DO $$
DECLARE p record;
BEGIN
	FOR p IN SELECT * FROM preds LOOP
		EXECUTE format('CREATE INDEX fz_%s ON events USING sift_col (kind) WITH (track_changes = off) WHERE %s', p.n, p.pred);
	END LOOP;
END $$;
CREATE FUNCTION unsound(pred text, filter text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE bad bool;
BEGIN
	EXECUTE format('SELECT EXISTS (SELECT 1 FROM events WHERE (%s) AND NOT coalesce((%s), false))', filter, pred) INTO bad;
	RETURN bad;
END $$;
SELECT count(*) > 500 AS pairs, count(*) FILTER (WHERE served) > 30 AS some_served, count(*) FILTER (WHERE NOT served) > 500 AS some_not,
       count(*) FILTER (WHERE served AND unsound(pred, filter)) AS unsound FROM (
  SELECT p.pred, f.filter, sift.projection_filter_served('events'::regclass, f.filter, format('fz_%s', p.n)::regclass) AS served FROM preds p, filters f) t;

-- 4. what stops being true: the index that served goes, or an index of the same name is made without a column; the status says broken, the queries take the normal plan, with the same rows
DROP INDEX fz_1, fz_2, fz_3, fz_4, fz_5, fz_6, fz_7, fz_8, fz_9, fz_10, fz_11, fz_12, fz_13, fz_14, fz_15, fz_16, fz_17, fz_18, fz_19, fz_20, fz_21, fz_22, fz_23, fz_24, fz_25, fz_26,
	fz_27, fz_28, fz_29, fz_30, fz_31, fz_32, fz_33, fz_34, fz_35, fz_36;
DROP INDEX events_wide;
SELECT projection, index, state, reason FROM sift.projection_status WHERE projection IN ('wide', 'by_kind', 'by_bucket', 'by_kind_qty') ORDER BY projection;
SELECT c.label, a.read_by, a.differences FROM classes c, LATERAL answered(c.stmt) a ORDER BY c.label;
CREATE INDEX events_wide ON events USING sift_col (dim_a_id, created_at, kind, status) WITH (track_changes = off, projection = 'wide, by_kind, by_bucket');
VACUUM (FREEZE, ANALYZE) events;
SELECT projection, index, state, reason FROM sift.projection_status WHERE projection IN ('wide', 'by_kind', 'by_bucket', 'by_kind_qty') ORDER BY projection, index;
SELECT c.label, a.read_by, a.differences FROM classes c, LATERAL answered(c.stmt) a ORDER BY c.label;
-- an index that went invalid (a failed concurrent build) does not serve, and a partial index that stopped covering the filter does not either
UPDATE pg_index SET indisvalid = false WHERE indexrelid = 'events_wide'::regclass;
SELECT projection, index, state, reason FROM sift.projection_status WHERE projection = 'by_kind';
UPDATE pg_index SET indisvalid = true WHERE indexrelid = 'events_wide'::regclass;
DROP INDEX tickets_live_index;
CREATE INDEX tickets_live_index ON tickets USING sift_col (dim_a_id, kind, qty, status) WITH (track_changes = off, projection = 'tickets_live, tickets_open') WHERE status = 'open';
SELECT projection, index, state, reason FROM sift.projection_status WHERE projection IN ('tickets_live', 'tickets_open') ORDER BY projection, index;

DROP TABLE events, tickets, classes, ticket_classes, atoms, preds, filters CASCADE;
DROP FUNCTION answered(text), served_case(text, text, text, text[], text, text, jsonb, jsonb), unsound(text, text);
TRUNCATE sift.projections;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
