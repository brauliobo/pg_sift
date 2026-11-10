-- The scope units replicate a row for each scope key of its member; past about 3 entries a row their growth costs more than their reads gain (docs/COL-SCOPE.md "Break-even"), so CREATE INDEX and
-- REINDEX refuse a scope above sift.scope_max_multiplicity (default 3) with its figure, and the setting is the override. The limit is on the average of the rows of the table.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SHOW sift.scope_max_multiplicity;
CREATE TABLE events (id int, dim_a_id int, created_at timestamptz, kind int, value_num numeric);
INSERT INTO events SELECT i, i % 2000 + 1, timestamptz '2026-01-01' + (i || ' min')::interval, i % 5, i FROM generate_series(1, 20000) i;
-- a scope relation whose members are in n keys (n = keys): 2000 members, so every row of the events has n entries
CREATE FUNCTION make_scope(rel text, keys numeric) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))', rel);
	EXECUTE format('INSERT INTO %I SELECT m, k FROM generate_series(1, 2000) m, LATERAL generate_series(1, floor($1)::int + (m %% 2 = 0 AND $1 %% 1 > 0)::int) k', rel) USING keys;
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (dim_a_id, tenant_id) WITH (track_changes = ''ctid'')', rel || '_journal', rel);
END $$;
CREATE FUNCTION try(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN EXECUTE q; RETURN 'built'; EXCEPTION WHEN OTHERS THEN RETURN sqlerrm; END $$;
SELECT make_scope('pairs_25', 2.5), make_scope('pairs_30', 3), make_scope('pairs_35', 3.5), make_scope('pairs_60', 6);

SELECT n AS keys_per_row, try(format('CREATE INDEX events_%s ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = ''tenant_id: dim_a_id -> pairs_%s(dim_a_id, tenant_id)'')', n, n)) AS default_limit
FROM (VALUES ('25'), ('30'), ('35'), ('60')) v(n) ORDER BY n;
SELECT indexrelid::regclass AS built FROM pg_index WHERE indrelid = 'events'::regclass ORDER BY 1::text;

-- the override builds the refused ones, and a lowered limit refuses what was accepted (a figure on the limit is accepted: the limit is the most)
SET sift.scope_max_multiplicity = 6;
SELECT try('CREATE INDEX events_35 ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = ''tenant_id: dim_a_id -> pairs_35(dim_a_id, tenant_id)'')') AS override_6;
SELECT try('CREATE INDEX events_60 ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = ''tenant_id: dim_a_id -> pairs_60(dim_a_id, tenant_id)'')') AS override_6_at_6;
RESET sift.scope_max_multiplicity;
SELECT try('REINDEX INDEX events_35') AS reindex_default;
SET sift.scope_max_multiplicity = 2;
SELECT try('REINDEX INDEX events_25') AS reindex_lowered;
RESET sift.scope_max_multiplicity;
SELECT sift.col_verify('events_25'::regclass, true) AS verify_25;
-- the hint
SET client_min_messages = notice;
DO $$
DECLARE h text;
BEGIN
	EXECUTE 'CREATE INDEX events_60b ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = ''tenant_id: dim_a_id -> pairs_60(dim_a_id, tenant_id)'')';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS h = PG_EXCEPTION_HINT;
	RAISE NOTICE 'hint: %', h;
END $$;
SET client_min_messages = warning;
DROP TABLE events;
DROP TABLE pairs_25, pairs_30, pairs_35, pairs_60;
DROP FUNCTION make_scope(text, numeric), try(text);
DROP EXTENSION pg_sift;
