-- sift.scope_create index options and inheritance: a jsonb column without documents needs options, a table with a sift_col index gives its columns and the options that describe them
-- to the copy, a caller's option replaces an inherited one, the copy is analyzed at once and reports that it was not vacuumed until it is.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at date NOT NULL, metrics_a jsonb, notes jsonb DEFAULT '[]', note text)
	WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 2 = 0 THEN 1 + (i / 2) % 60 ELSE 10 * ((i / 2) % 6) + 1 + (i / 12) % 5 END, date '2026-01-01' + i % 300, jsonb_build_object('mhead', i % 10, 'service', i % 7), '[]', 'n' FROM generate_series(1, 6000) i;
CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = false);
-- member e has 0 (e % 10 = 0), 1, 2, 3, 4 or 6 keys
INSERT INTO pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) k;
SET client_min_messages = warning;

-- documents without a leaf cannot be copied as they are; the options give the paths
SELECT sift.scope_create('by_options', 'facts', 'dim_a_id', 'pairs', 'scope_id');
SELECT sift.scope_create('by_options', 'facts', 'dim_a_id', 'pairs', 'scope_id', options => 'paths = ''metrics_a.* float8, notes.k int4''') AS copy_rows;
SELECT reloptions FROM pg_class WHERE relname = 'by_options_col';

-- a fresh copy is analyzed, not vacuumed: sift.scope_status says so until VACUUM
SELECT vacuumed, all_visible_pages, pages > 0 AS has_pages FROM sift.scope_status WHERE copy = 'public.by_options';
VACUUM by_options;
SELECT vacuumed, all_visible_pages * 10 >= pages * 9 AS nearly_all_visible FROM sift.scope_status WHERE copy = 'public.by_options';
SELECT sift.scope_drop('by_options');

-- a table with a sift_col index: the copy carries its columns and the options that describe them, not the options keyed on the table
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, metrics_a)
	WITH (paths = 'metrics_a.* float8', dict_lookup = off, array_lengths = off, key_cut = off, track_changes = off);
SELECT sift.scope_create('by_index', 'facts', 'dim_a_id', 'pairs', 'scope_id') AS copy_rows;
SELECT columns FROM sift.scope_copies WHERE copy = 'public.by_index';
SELECT reloptions FROM pg_class WHERE relname = 'by_index_col';
VACUUM (FREEZE, ANALYZE) by_index;
-- the map is read by the copy's index
EXPLAIN (COSTS OFF) SELECT r.key, avg((r.value #>> '{}')::float) FROM by_index f JOIN LATERAL (SELECT key, value FROM jsonb_each(f.metrics_a)) r ON true
	WHERE f.scope_id = 1 GROUP BY r.key;
SELECT (SELECT md5(string_agg(t::text, '|' ORDER BY t::text)) FROM (SELECT r.key, avg((r.value #>> '{}')::float) FROM by_index f
			JOIN LATERAL (SELECT key, value FROM jsonb_each(f.metrics_a)) r ON true WHERE f.scope_id = 1 GROUP BY r.key) t)
	 = (SELECT md5(string_agg(t::text, '|' ORDER BY t::text)) FROM (SELECT r.key, avg((r.value #>> '{}')::float) FROM facts f JOIN pairs p USING (dim_a_id)
			JOIN LATERAL (SELECT key, value FROM jsonb_each(f.metrics_a)) r ON true WHERE p.scope_id = 1 GROUP BY r.key) t) AS same_rows;
SELECT sift.scope_drop('by_index');

-- an option of the caller replaces the inherited one (key_order, the scope key and order column); columns replaces the index columns
SELECT sift.scope_create('overridden', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['created_at', 'metrics_a'],
						 options => 'array_lengths = on, key_order = ''scope_id''') AS copy_rows;
SELECT columns FROM sift.scope_copies WHERE copy = 'public.overridden';
SELECT reloptions FROM pg_class WHERE relname = 'overridden_col';
SELECT sift.scope_drop('overridden');
-- an inherited option names the columns of the table's index: leaving one out needs that option from the caller too
SELECT sift.scope_create('overridden', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['created_at', 'note']);
SELECT sift.scope_create('overridden', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['created_at', 'note'], options => 'paths = '''' ') AS copy_rows;
SELECT sift.scope_drop('overridden');

-- a partitioned copy reports the pages of its partitions
SELECT sift.scope_create('by_part', 'facts', 'dim_a_id', 'pairs', 'scope_id', partition => true) AS copy_rows;
SELECT vacuumed, pages > 0 AS has_pages FROM sift.scope_status WHERE copy = 'public.by_part';
SELECT sift.scope_drop('by_part');

DROP INDEX facts_col;
DROP TABLE facts, pairs;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
