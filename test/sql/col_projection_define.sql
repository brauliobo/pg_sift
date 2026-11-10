-- Named projections (sift.projection_create, sift.projection_index): a definition of what a sift_col index stores, written once, checked on an empty copy of the table,
-- and turned into the indexes by one call that builds the index or says that an existing one already holds it. The names are listed in the index option projection,
-- the definitions are a configuration table with a guard: a definition that an index lists cannot be deleted, nor change its table, columns or filter.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind int, qty int, status text, created_at timestamptz);
INSERT INTO events SELECT i, i % 40, i % 5, i % 13, (ARRAY['new', 'open', 'held'])[1 + i % 3], timestamptz '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 3000) i;
CREATE VIEW events_view AS SELECT * FROM events;
VACUUM (FREEZE, ANALYZE) events;

-- a definition is refused when the empty copy cannot take it, or when it sets what belongs to the index
SELECT t.label, sift_helpers.statement_outcome(t.stmt) AS outcome FROM (VALUES
  ('ok',               $$SELECT sift.projection_create('by_dim', 'events', ARRAY['dim_a_id', 'kind', '(qty / 10)'])$$),
  ('same name',        $$SELECT sift.projection_create('by_dim', 'events', ARRAY['kind'])$$),
  ('bad name',         $$SELECT sift.projection_create('By Dim', 'events', ARRAY['kind'])$$),
  ('a view',           $$SELECT sift.projection_create('on_view', 'events_view', ARRAY['kind'])$$),
  ('no columns',       $$SELECT sift.projection_create('empty', 'events', ARRAY[]::text[])$$),
  ('unknown column',   $$SELECT sift.projection_create('bad_column', 'events', ARRAY['nope'])$$),
  ('volatile column',  $$SELECT sift.projection_create('volatile', 'events', ARRAY['(random())'])$$),
  ('bad filter',       $$SELECT sift.projection_create('bad_filter', 'events', ARRAY['kind'], filter => 'nope = 1')$$),
  ('bad rollups',      $$SELECT sift.projection_create('bad_rollup', 'events', ARRAY['kind'], rollups => 'nope')$$),
  ('layout option',    $$SELECT sift.projection_create('layout', 'events', ARRAY['kind'], options => '{"key_order": "kind"}')$$),
  ('rollup option',    $$SELECT sift.projection_create('rollup', 'events', ARRAY['kind'], options => '{"rollup_columns": "kind"}')$$),
  ('unknown option',   $$SELECT sift.projection_create('unknown', 'events', ARRAY['kind'], options => '{"nope": "1"}')$$)
) t(label, stmt);
SELECT name, source, leaves, rollups, filter, options, canonical, canonical_filter FROM sift.projections ORDER BY name;

SELECT sift.projection_create('by_dim_open', 'events', ARRAY['dim_a_id', 'kind', 'qty', 'status'], rollups => 'dim_a_id, kind', filter => $$status = 'open'$$);
SELECT sift.projection_create('by_status', 'events', ARRAY['status', 'created_at'], options => '{"superunit": 8}');
SELECT sift.projection_create('by_dim_open_a', 'events', ARRAY['dim_a_id', 'kind', 'created_at'], rollups => 'dim_a_id, kind', filter => $$status = 'open'$$);

-- projection_index refuses what it cannot make
SELECT t.label, sift_helpers.statement_outcome(t.stmt) AS outcome FROM (VALUES
  ('no name',        $$SELECT sift.projection_index('i', ARRAY[]::text[])$$),
  ('not defined',    $$SELECT sift.projection_index('i', ARRAY['nope'])$$),
  ('differ',         $$SELECT sift.projection_index('i', ARRAY['by_dim', 'by_status'])$$),
  ('layout rollups', $$SELECT sift.projection_index('i', ARRAY['by_dim'], '{"rollup_columns": "kind"}')$$),
  ('layout name',    $$SELECT sift.projection_index('i', ARRAY['by_dim'], '{"projection": "x"}')$$),
  ('bad layout',     $$SELECT sift.projection_index('i', ARRAY['by_dim'], '{"key_order": "status"}')$$)
) t(label, stmt);

-- the index of a projection lists it, in the order of the names; its columns are the union of the projections' columns without repeats; a second call is the same index
SELECT sift.projection_index('events_dim', ARRAY['by_dim'], '{"key_order": "dim_a_id", "track_changes": "off"}');
SELECT sift.projection_index('events_dim', ARRAY['by_dim'], '{"key_order": "dim_a_id", "track_changes": "off"}');
SELECT sift.projection_index('events_open', ARRAY['by_dim_open', 'by_dim_open_a'], '{"track_changes": "off"}');
SELECT sift.projection_index('events_status', ARRAY['by_status']);
SELECT i.relname, ARRAY(SELECT pg_get_indexdef(i.oid, k, true) FROM generate_series(1, i.relnatts) k) AS leaves, sift.projection_option(i.oid, 'projection') AS projection,
       sift.projection_option(i.oid, 'rollup_columns') AS rollup, sift.projection_option(i.oid, 'superunit') AS superunit
FROM pg_class i WHERE i.relname IN ('events_dim', 'events_open', 'events_status') ORDER BY i.relname;
SELECT projection, index, state, reason, shared FROM sift.projection_status ORDER BY projection, index;
SELECT p.name, array_agg(i::regclass::text) AS indexes FROM sift.projections p, sift.projection_indexes(p.name) i GROUP BY p.name ORDER BY p.name;

-- the option names are read by sift.projection_status, not by CREATE INDEX: a name nobody defined is a broken projection, a name of another shape is an error
CREATE INDEX events_unknown ON events USING sift_col (kind, qty) WITH (projection = 'not_defined', track_changes = off);
SELECT t.label, sift_helpers.statement_outcome(t.stmt) AS outcome FROM (VALUES
  ('upper case', $$ALTER INDEX events_unknown SET (projection = 'By_Dim')$$),
  ('empty name', $$ALTER INDEX events_unknown SET (projection = 'a,,b')$$),
  ('a digit',    $$ALTER INDEX events_unknown SET (projection = '1a')$$),
  ('a list',     $$ALTER INDEX events_unknown SET (projection = 'a, b')$$)
) t(label, stmt);
SELECT projection, source, index, state, reason FROM sift.projection_status WHERE index = 'events_unknown' ORDER BY projection;
DROP INDEX events_unknown;

-- a held definition keeps its table, columns and filter; its rollups and options can change
SET client_min_messages = notice;
SELECT t.label, sift_helpers.statement_outcome(t.stmt) AS outcome FROM (VALUES
  ('delete',           $$DELETE FROM sift.projections WHERE name = 'by_dim'$$),
  ('drop',             $$SELECT sift.projection_drop('by_dim')$$),
  ('drop unknown',     $$SELECT sift.projection_drop('nope')$$),
  ('truncate',         $$TRUNCATE sift.projections$$),
  ('rename',           $$UPDATE sift.projections SET name = 'by_dim2' WHERE name = 'by_dim'$$),
  ('columns',          $$UPDATE sift.projections SET leaves = ARRAY['kind'] WHERE name = 'by_dim'$$),
  ('filter',           $$UPDATE sift.projections SET filter = 'kind = 1' WHERE name = 'by_dim'$$),
  ('replace columns',  $$SELECT sift.projection_replace('by_dim', ARRAY['dim_a_id', 'kind'])$$),
  ('replace filter',   $$SELECT sift.projection_replace('by_dim', ARRAY['dim_a_id', 'kind', '(qty / 10)'], filter => 'kind = 1')$$),
  ('replace unknown',  $$SELECT sift.projection_replace('nope', ARRAY['kind'])$$),
  ('same definition',  $$SELECT sift.projection_replace('by_dim', ARRAY['dim_a_id', 'kind', '(qty / 10)'])$$)
) t(label, stmt);
SELECT sift_helpers.statement_outcome($$SELECT sift.projection_replace('by_dim_open', ARRAY['dim_a_id', 'kind', 'qty', 'status'], rollups => 'dim_a_id', filter => 'status = ''open''')$$) AS bad_rollup;
-- new rollups and options reach an index that lists only this projection (it needs REINDEX); an index that lists others too is left as it is, and the status compares it
\set VERBOSITY default
SELECT sift.projection_replace('by_status', ARRAY['status', 'created_at'], options => '{"superunit": 4}');
\set VERBOSITY terse
SELECT sift.projection_replace('by_dim_open', ARRAY['dim_a_id', 'kind', 'qty', 'status'], rollups => 'dim_a_id, kind: qty', filter => $$status = 'open'$$);
SET client_min_messages = warning;
SELECT projection, index, state, reason FROM sift.projection_status ORDER BY projection, index;
SELECT sift.projection_option(oid, 'superunit') AS superunit, sift.projection_option(oid, 'rollup_columns') AS rollup FROM pg_class WHERE relname IN ('events_status', 'events_open') ORDER BY relname;

-- an index that no longer lists the projection frees it
ALTER INDEX events_dim RESET (projection);
SELECT sift_helpers.statement_outcome($$SELECT sift.projection_replace('by_dim', ARRAY['dim_a_id', 'kind'])$$) AS replace_free;
SELECT projection, index, state, reason FROM sift.projection_status WHERE projection = 'by_dim';

-- the statements of the recipe: CREATE INDEX CONCURRENTLY for an index of another layout, the ALTER INDEX when an existing index serves the projection
SELECT sift.projection_index_sql('events_dim_kind', ARRAY['by_dim'], '{"key_order": "kind", "track_changes": "off"}') AS ddl \gset
SELECT :'ddl' AS concurrently;
:ddl ;
SELECT sift.projection_index_sql('events_dim_plain', ARRAY['by_dim'], '{"track_changes": "off"}', false) AS plain;
SELECT sift.projection_index_sql('events_dim2', ARRAY['by_dim'], '{"key_order": "dim_a_id", "track_changes": "off"}') AS served;
SELECT sift_helpers.statement_outcome($$SELECT sift.projection_index_sql('events_dim', ARRAY['by_dim_open'])$$) AS name_taken;
SELECT projection, index, state FROM sift.projection_status WHERE projection = 'by_dim';

-- the guard lets go when the last index is gone
DROP INDEX events_open, events_status, events_dim, events_dim_kind;
SELECT sift.projection_drop(name) FROM sift.projections ORDER BY name;
SELECT count(*) AS definitions FROM sift.projections;
DROP TABLE events CASCADE;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
