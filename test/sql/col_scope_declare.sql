-- The reloption scope of a sift_col index: 'key_name: fk -> [schema.]relation(member, scope_key)'. CREATE INDEX, REINDEX and ALTER INDEX resolve it: the index depends
-- on the unique btree of the scope relation on (member, scope_key), has the derived leaf "<key_name>#scope" (NULL in every row the entity segments hold) and is
-- column format minor 45. The text is the record (pg_dump writes it). The build writes the scope segments too (test col_scope_build): the entity segments answer as for an index without the scope.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT i, 1 + i % 5 FROM generate_series(1, 500) i;
INSERT INTO tenant_scope SELECT i, 6 FROM generate_series(1, 500, 7) i;
-- the journal index of the scope relation: a scope index needs it (member first, track_changes = 'ctid')
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind int NOT NULL, label text, value_num int NOT NULL, pad int)
	WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 500, timestamptz '2026-01-01' + i * interval '1 minute', i % 7, 'l' || i % 11, i, 0 FROM generate_series(1, 3000) i;

CREATE FUNCTION edges(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce(string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text), 'none') FROM pg_depend
   WHERE classid = 'pg_class'::regclass AND objid = idx AND refclassid = 'pg_class'::regclass AND deptype = 'n'
	 AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i' $$;
CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;
CREATE FUNCTION answers(sql text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE r text; BEGIN EXECUTE format('SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (%s) t', sql) INTO r; RETURN r; END $$;

-- the relations a scope can name, and what is wrong with the others
CREATE TABLE pairs_nullable (m int, s int, UNIQUE (m, s));
CREATE TABLE pairs_text (m text NOT NULL, s int NOT NULL, PRIMARY KEY (m, s));
CREATE TABLE pairs_plain (m int NOT NULL, s int NOT NULL);
CREATE TABLE pairs_three (m int NOT NULL, s int NOT NULL, x int NOT NULL, PRIMARY KEY (m, s, x));
CREATE TABLE pairs_partial (m int NOT NULL, s int NOT NULL);
CREATE UNIQUE INDEX pairs_partial_i ON pairs_partial (m, s) WHERE s > 0;
CREATE TABLE pairs_deferrable (m int NOT NULL, s int NOT NULL, UNIQUE (m, s) DEFERRABLE);
CREATE TABLE pairs_include (m int NOT NULL, s int NOT NULL, x int);
CREATE UNIQUE INDEX pairs_include_i ON pairs_include (m, s) INCLUDE (x);
CREATE TABLE pairs_part (m int NOT NULL, s int NOT NULL, PRIMARY KEY (m, s)) PARTITION BY RANGE (m);
CREATE VIEW pairs_view AS SELECT dim_a_id AS m, tenant_id AS s FROM tenant_scope;
CREATE TEMP TABLE bad_scopes (n serial, why text, opt text);
INSERT INTO bad_scopes (why, opt) VALUES
	('no colon', 'tenant_id dim_a_id -> tenant_scope(dim_a_id, tenant_id)'),
	('no arrow', 'tenant_id: dim_a_id tenant_scope(dim_a_id, tenant_id)'),
	('no parenthesis', 'tenant_id: dim_a_id -> tenant_scope'),
	('one column', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id)'),
	('three columns', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id, x)'),
	('unclosed', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id'),
	('trailing text', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) carries x'),
	('empty scope after ;', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id);'),
	('quote not closed', 'tenant_id: "dim_a_id -> tenant_scope(dim_a_id, tenant_id)'),
	('twice the same name', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id); tenant_id: kind -> tenant_scope(dim_a_id, tenant_id)'),
	('five scopes', 'a: dim_a_id -> tenant_scope(dim_a_id, tenant_id); b: dim_a_id -> tenant_scope(dim_a_id, tenant_id); c: dim_a_id -> tenant_scope(dim_a_id, tenant_id); d: dim_a_id -> tenant_scope(dim_a_id, tenant_id); e: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'),
	('fk not in the index', 'tenant_id: pad -> tenant_scope(dim_a_id, tenant_id)'),
	('fk not a column', 'tenant_id: nothing -> tenant_scope(dim_a_id, tenant_id)'),
	('fk not an integer', 'tenant_id: label -> tenant_scope(dim_a_id, tenant_id)'),
	('name of a column of the index', 'kind: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'),
	('relation missing', 'tenant_id: dim_a_id -> nowhere(dim_a_id, tenant_id)'),
	('relation is the table', 'tenant_id: dim_a_id -> events(id, kind)'),
	('relation is a view', 'tenant_id: dim_a_id -> pairs_view(m, s)'),
	('relation is partitioned', 'tenant_id: dim_a_id -> pairs_part(m, s)'),
	('member column missing', 'tenant_id: dim_a_id -> tenant_scope(nothing, tenant_id)'),
	('scope key column missing', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, nothing)'),
	('same column twice', 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, dim_a_id)'),
	('member not an integer', 'tenant_id: dim_a_id -> pairs_text(m, s)'),
	('scope key not an integer', 'tenant_id: dim_a_id -> pairs_text(s, m)'),
	('nullable columns', 'tenant_id: dim_a_id -> pairs_nullable(m, s)'),
	('no unique btree', 'tenant_id: dim_a_id -> pairs_plain(m, s)'),
	('unique on three columns', 'tenant_id: dim_a_id -> pairs_three(m, s)'),
	('partial unique', 'tenant_id: dim_a_id -> pairs_partial(m, s)'),
	('deferrable unique', 'tenant_id: dim_a_id -> pairs_deferrable(m, s)'),
	('unique with an included column', 'tenant_id: dim_a_id -> pairs_include(m, s)');
SELECT n, why, try(format('CREATE INDEX bad ON events USING sift_col (id, dim_a_id, created_at, kind, label, value_num) WITH (scope = %L)', opt)) AS result
	FROM bad_scopes ORDER BY n;
SELECT count(*) AS leftover_indexes FROM pg_class WHERE relname = 'bad';

-- the option on its own, the settings
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (scope = 'off');
DROP INDEX bad;
SHOW sift.agg_scope;
SHOW sift.scope_patch_keys;
SELECT try('SET sift.scope_patch_keys = 0');
SELECT try('SET sift.scope_patch_keys = 1048577');

-- the declaration: a dependency on the btree, the derived leaf, minor 45
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, label, value_num)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT edges('events_col') AS edges;
SELECT leaf, path, type, kind FROM sift.col_leaves('events_col') ORDER BY leaf;
SELECT format, rows, kind FROM sift.col_segments('events_col') WHERE kind = 'immutable';
SELECT o AS option FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'events_col' AND o LIKE 'scope=%';
-- a scope and no scope answer alike
CREATE TABLE events_plain AS SELECT * FROM events;
CREATE INDEX events_plain_col ON events_plain USING sift_col (id, dim_a_id, created_at, kind, label, value_num);
SELECT (SELECT format FROM sift.col_segments('events_plain_col') WHERE kind = 'immutable') AS plain_format;
SELECT answers('SELECT kind, count(*), sum(value_num), min(created_at), max(created_at) FROM events GROUP BY kind') =
	   answers('SELECT kind, count(*), sum(value_num), min(created_at), max(created_at) FROM events_plain GROUP BY kind') AS same_groups,
	   answers('SELECT dim_a_id, count(*) FROM events WHERE value_num > 2000 GROUP BY dim_a_id') =
	   answers('SELECT dim_a_id, count(*) FROM events_plain WHERE value_num > 2000 GROUP BY dim_a_id') AS same_filter,
	   answers('SELECT id, value_num FROM events WHERE label = ''l3'' ORDER BY created_at LIMIT 20') =
	   answers('SELECT id, value_num FROM events_plain WHERE label = ''l3'' ORDER BY created_at LIMIT 20') AS same_top;
-- rows written after the build (the write buffer) have no derived key either
INSERT INTO events SELECT i, 1 + i % 500, timestamptz '2026-03-01' + i * interval '1 minute', i % 7, 'l' || i % 11, i, 0 FROM generate_series(3001, 3100) i;
INSERT INTO events_plain SELECT * FROM events WHERE id > 3000;
SELECT answers('SELECT kind, count(*), sum(value_num) FROM events GROUP BY kind') = answers('SELECT kind, count(*), sum(value_num) FROM events_plain GROUP BY kind') AS same_after_insert;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT answers('SELECT kind, count(*), sum(value_num) FROM events GROUP BY kind') = answers('SELECT kind, count(*), sum(value_num) FROM events_plain GROUP BY kind') AS same_after_flush;
SELECT format, kind FROM sift.col_segments('events_col') WHERE kind = 'immutable' ORDER BY gen;
SELECT sift.col_verify('events_col', true);
DROP TABLE events_plain;

-- the text is the record: pg_dump writes it and a restore makes the same index
SELECT regexp_replace(pg_get_indexdef('events_col'::regclass), '^.* WITH', 'WITH') AS definition;
SELECT pg_get_indexdef('events_col'::regclass) AS ddl \gset
DROP INDEX events_col;
SELECT edges('events_col') AS after_drop;
SELECT :'ddl' AS restore \gexec
SELECT edges('events_col') AS after_restore;
SELECT leaf, path FROM sift.col_leaves('events_col') WHERE path LIKE '%#scope';

-- REINDEX, REINDEX CONCURRENTLY and ALTER INDEX of another option keep one dependency; the scope index and the btree can both be rebuilt
REINDEX INDEX events_col;
SELECT edges('events_col') AS after_reindex;
REINDEX INDEX CONCURRENTLY events_col;
SELECT edges('events_col') AS after_reindex_concurrently, (SELECT count(*) FROM pg_class WHERE relname LIKE 'events_col%') AS indexes;
ALTER INDEX events_col SET (superunit = 8);
SELECT edges('events_col') AS after_other_option;
REINDEX INDEX CONCURRENTLY tenant_scope_pkey;
SELECT edges('events_col') AS after_btree_reindex_concurrently;
REINDEX INDEX events_col;
SELECT format, kind FROM sift.col_segments('events_col') WHERE kind = 'immutable';

-- off removes the declaration (the leaf stays in the segments until the next REINDEX), the text brings it back, RESET removes it again
ALTER INDEX events_col SET (scope = 'off');
SELECT edges('events_col') AS after_off;
ALTER INDEX events_col SET (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT edges('events_col') AS after_set;
SELECT try('ALTER INDEX events_col SET (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, nokey)'')');
SELECT edges('events_col') AS after_bad_set;
ALTER INDEX events_col RESET (scope);
SELECT edges('events_col') AS after_reset;
REINDEX INDEX events_col;
SELECT format, kind FROM sift.col_segments('events_col') WHERE kind = 'immutable';
SELECT count(*) AS scope_leaves FROM sift.col_leaves('events_col') WHERE path LIKE '%#scope';
ALTER INDEX events_col SET (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

-- the btree and the relation cannot go under the index; CASCADE takes the index with them
SELECT try('DROP INDEX tenant_scope_pkey');
SELECT try('ALTER TABLE tenant_scope DROP CONSTRAINT tenant_scope_pkey');
SELECT try('DROP TABLE tenant_scope');
SELECT try('DROP TABLE tenant_scope CASCADE');
SELECT count(*) AS scope_indexes FROM pg_class WHERE relname = 'events_col';

-- a relation in another schema is named with its schema, an unqualified one is in the schema of the table; two scopes, a quoted name, and a connection beside them
CREATE SCHEMA other;
CREATE TABLE other.region_scope (dim_a_id int NOT NULL, region_id int NOT NULL, UNIQUE (region_id, dim_a_id));
INSERT INTO other.region_scope SELECT i, i % 3 FROM generate_series(1, 500) i;
CREATE INDEX region_scope_journal ON other.region_scope USING sift_col (dim_a_id, region_id) WITH (track_changes = 'ctid');
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id));
INSERT INTO tenant_scope SELECT i, 1 + i % 5 FROM generate_series(1, 500) i;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL);
INSERT INTO dim_a SELECT i, i % 10 FROM generate_series(1, 500) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id);
SELECT try('CREATE INDEX bad ON events USING sift_col (id, dim_a_id, kind) WITH (scope = ''r: dim_a_id -> region_scope(dim_a_id, region_id)'')') AS unqualified;
CREATE INDEX events_two ON events USING sift_col (id, dim_a_id, kind, value_num)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id); "Region Key": dim_a_id -> other.region_scope(dim_a_id, region_id)',
		  connect = 'dim_a_id -> dim_a(id)');
SELECT edges('events_two') AS edges;
SELECT leaf, path, type FROM sift.col_leaves('events_two') ORDER BY leaf;
SELECT format, rows FROM sift.col_segments('events_two') WHERE kind = 'immutable';
SELECT sift.col_verify('events_two', true);
ALTER INDEX events_two SET (connect = 'off');
SELECT edges('events_two') AS after_connect_off;
ALTER INDEX events_two SET (connect = 'dim_a_id -> dim_a(id)');
ALTER INDEX events_two SET (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT edges('events_two') AS after_one_scope_off;
ALTER INDEX events_two RESET (scope);
SELECT edges('events_two') AS after_scope_reset;
DROP INDEX events_two;
DROP SCHEMA other CASCADE;
DROP TABLE tenant_scope, dim_a, events, pairs_nullable, pairs_text, pairs_plain, pairs_three, pairs_partial, pairs_deferrable, pairs_include, pairs_part CASCADE;
DROP FUNCTION edges(regclass);
DROP FUNCTION try(text);
DROP FUNCTION answers(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
