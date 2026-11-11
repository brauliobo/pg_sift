-- sift.scope_create index options and inheritance: a jsonb column without documents needs options, a table with a sift_col index gives its columns and the options that describe them
-- to the copy, a caller's option replaces an inherited one, the copy is analyzed at once and reports that it was not vacuumed until it is.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at date NOT NULL, metrics_a jsonb, notes jsonb DEFAULT '[]', note text)
	WITH (autovacuum_enabled = false);
CREATE INDEX ON facts (dim_a_id);
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
-- the paths of the table's index name jsonb columns the copy leaves out: none is left, so the copy does not inherit them; the caller's option still replaces
SELECT sift.scope_create('overridden', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['created_at', 'note']) AS copy_rows;
SELECT reloptions FROM pg_class WHERE relname = 'overridden_col';
SELECT sift.scope_drop('overridden');
SELECT sift.scope_create('overridden', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['created_at', 'note'], options => 'paths = '''' ') AS copy_rows;
SELECT sift.scope_drop('overridden');

-- a partitioned copy reports the pages of its partitions
SELECT sift.scope_create('by_part', 'facts', 'dim_a_id', 'pairs', 'scope_id', partition => true) AS copy_rows;
SELECT vacuumed, pages > 0 AS has_pages FROM sift.scope_status WHERE copy = 'public.by_part';
SELECT sift.scope_drop('by_part');

-- an inherited option that names columns is restricted to the carried ones: an entry naming another column goes, an option with nothing left is not inherited
-- (a NOTICE lists it), a word (auto, off, all) and an option that names no column stay; the copy's index is built from the result
CREATE SCHEMA inh;
CREATE TABLE inh.pairs (entity int NOT NULL, tenant int NOT NULL, PRIMARY KEY (tenant, entity));
INSERT INTO inh.pairs SELECT g, 1 + g % 3 FROM generate_series(1, 20) g;
-- a table with an index of the option (the paths of the documents too, which a jsonb column needs), its copy of the columns, and the option the copy's index got
CREATE FUNCTION inh.try(n int, opt text, val text, cols name[]) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE inh.t%s (id int PRIMARY KEY, entity int NOT NULL, day date, due date, kind text, amt int, qty int, doc jsonb, extra jsonb)', n);
	EXECUTE format('INSERT INTO inh.t%s SELECT g, 1 + g %% 20, date ''2026-01-01'' + g %% 30, date ''2026-02-01'' + g %% 30, ''k'' || g %% 5, g, g %% 7, ''{"a": 1.5}'',
					''[{"k": "x", "n": 1}]'' FROM generate_series(1, 200) g', n);
	EXECUTE format('CREATE INDEX ON inh.t%s (entity)', n);
	EXECUTE format('CREATE INDEX t%s_col ON inh.t%s USING sift_col (id, entity, day, due, kind, amt, qty, doc, extra) WITH (paths = %L%s)', n, n,
				   CASE WHEN opt = 'paths' THEN val ELSE 'doc.a float8, extra[]{k text, n int4}' END, CASE WHEN opt = 'paths' THEN '' ELSE format(', %s = %L', opt, val) END);
	PERFORM sift.scope_create(format('inh.t%s_c', n), format('inh.t%s', n)::regclass, 'entity', 'inh.pairs', 'tenant', columns => cols);
	RETURN (SELECT substr(o, strpos(o, '=') + 1) FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = format('inh.t%s_c_col', n)::regclass AND o LIKE opt || '=%');
END $$;
SELECT n, opt, val AS inherited, cols, sift._scope_restrict(opt, val, cols || ARRAY['id', 'entity']) AS restricted, inh.try(n, opt, val, cols) AS built
FROM (VALUES
	(1, 'paths', 'doc.a float8, extra[]{k text, n int4}', ARRAY['doc']),
	(2, 'paths', 'doc.a float8, extra[]{k text, n int4}', ARRAY['extra']),
	(3, 'paths', 'doc.a float8, extra[]{k text, n int4}', ARRAY['doc', 'extra']),
	(4, 'paths', 'doc.a float8, extra[]{k text, n int4}', ARRAY['kind']),
	(5, 'compression', 'auto; kind=zstd:3; qty=lz4', ARRAY['kind']),
	(6, 'compression', 'kind=zstd:3; qty=lz4', ARRAY['amt']),
	(7, 'preagg', 'cube(kind: amt, qty; entity: amt)', ARRAY['kind', 'amt']),
	(8, 'preagg', 'cube(kind: qty; entity: qty)', ARRAY['kind', 'amt']),
	(9, 'preagg', 'off', ARRAY['kind']),
	(10, 'rollup_columns', 'kind, day: amt, qty', ARRAY['kind', 'day', 'amt']),
	(11, 'rollup_columns', 'kind, day: amt, qty', ARRAY['kind', 'amt']),
	(12, 'rollup_columns', 'kind, day: qty', ARRAY['kind', 'day', 'amt']),
	(13, 'rollup_columns', 'off', ARRAY['kind']),
	(14, 'rollup_leaves', 'extra[].k, extra[].n', ARRAY['extra']),
	(15, 'rollup_leaves', 'extra[].k', ARRAY['doc']),
	(16, 'calendar_columns', 'day, due', ARRAY['day']),
	(17, 'calendar_columns', 'day, due', ARRAY['kind']),
	(18, 'calendar_columns', 'off', ARRAY['kind']),
	(19, 'hll_columns', 'kind, qty', ARRAY['qty']),
	(20, 'hll_columns', 'kind, qty', ARRAY['amt']),
	(21, 'return_columns', 'kind', ARRAY['kind']),
	(22, 'return_columns', 'kind', ARRAY['amt']),
	(23, 'superunit', '4', ARRAY['amt']),
	(24, 'cube_minmax', 'false', ARRAY['amt']),
	(25, 'paths', 'doc."a,b" float8, extra[]{k text, n int4}', ARRAY['doc'])
) AS c(n, opt, val, cols) ORDER BY n;
-- the copy chooses again an option that is not inherited (case 11: its own rollup_columns); the option that is left out says so
SET client_min_messages = notice;
SELECT sift.scope_create('inh.notice_c', 'inh.t4', 'entity', 'inh.pairs', 'tenant', columns => ARRAY['kind', 'qty']) AS copy_rows;
SET client_min_messages = warning;
-- the case of a path of a jsonb column the copy leaves out
CREATE TABLE inh.events (id int PRIMARY KEY, entity int, day date, doc jsonb, extra jsonb);
INSERT INTO inh.events SELECT g, 1 + g % 20, date '2026-01-01' + g % 30, '{"a": 1.5}', '[]' FROM generate_series(1, 500) g;
CREATE INDEX ON inh.events (entity);
CREATE INDEX events_col ON inh.events USING sift_col (id, entity, day, doc, extra) WITH (key_order = 'entity, day', paths = 'doc.a float8, extra[]{k text}');
SELECT sift.scope_create('inh.events_by_tenant', 'inh.events', 'entity', 'inh.pairs', 'tenant', columns => ARRAY['id', 'entity', 'day', 'doc']) AS copy_rows;
SELECT reloptions FROM pg_class WHERE oid = 'inh.events_by_tenant_col'::regclass;
SELECT count(sift.scope_drop(replace(copy, 'public.', ''))) AS dropped FROM sift.scope_copies WHERE copy LIKE 'inh.%';
DROP SCHEMA inh CASCADE;

DROP INDEX facts_col;
DROP TABLE facts, pairs;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
