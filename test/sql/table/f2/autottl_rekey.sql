-- A column of a table that has a Format 2.0 sift_col index is renamed and the index follows (src/col/names.c, sift_col_rekey): the options that name the column (key_order), the connect and
-- scope options of the indexes that name it, and the metapage, which takes the hash of the leaves under the new names; no part is written again. The index is verified, answers as the table
-- does and takes writes under the new names. And the commands of the tiering and of the TTL name the partitions when they are run on a partitioned parent.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE EXTENSION IF NOT EXISTS pg_sift_helpers;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET max_parallel_workers_per_gather = 0;

-- a partitioned parent holds no parts
CREATE TABLE pt (id int, at timestamptz NOT NULL) PARTITION BY RANGE (at);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM ('2030-01-01') TO ('2031-01-01') USING sift_table;
SELECT sift.set_ttl('pt', 'at', interval '7 days');
SELECT sift.set_tier_policy('pt', 'file:///nonexistent', interval '1 day');
SELECT sift.clear_tier_policy('pt');
SELECT sift.set_ttl('pt1', 'at', interval '7 days');
SELECT sift.set_ttl('pt1', 'at', NULL);
DROP TABLE pt;

CREATE FUNCTION rk_opt(idx regclass, key text) RETURNS text LANGUAGE sql STABLE AS
$$ SELECT substr(o, length(key) + 2) FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = idx AND starts_with(o, key || '=') $$;
CREATE FUNCTION rk_attrs(idx regclass) RETURNS text LANGUAGE sql STABLE AS
$$ SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute WHERE attrelid = idx AND attnum > 0 $$;
CREATE FUNCTION rk_sound(idx regclass) RETURNS boolean LANGUAGE sql AS
$$ SELECT (SELECT count(*) FROM sift.check(idx)) = 0 AND (SELECT state = 'ready' AND NOT quarantined FROM sift.status(idx)) $$;
-- a dump and a restore of the index: its definition run again; true when the same options come back
CREATE FUNCTION rk_redo(idx regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	def    text   := pg_get_indexdef(idx);
	name   text   := idx::text;
	before text[] := (SELECT reloptions FROM pg_class WHERE oid = idx);
BEGIN
	EXECUTE format('DROP INDEX %s', idx);
	EXECUTE def;
	RETURN before = (SELECT reloptions FROM pg_class WHERE oid = name::regclass);
END $$;

-- plain columns and the order of the index, which names them
CREATE TABLE rk (id int PRIMARY KEY, dim_id int NOT NULL, at timestamptz NOT NULL, kind int NOT NULL, val int NOT NULL, note text) WITH (autovacuum_enabled = off);
INSERT INTO rk SELECT i, 1 + i % 50, timestamptz '2030-01-01' + i * interval '1 minute', i % 7, i, 'n' || i FROM generate_series(1, 3000) i;
CREATE INDEX rk_col ON rk USING sift_col (id, dim_id, at, kind, val, note) WITH (key_order = 'dim_id, at');
ANALYZE rk;
SELECT rk_attrs('rk_col') AS attrs, rk_opt('rk_col', 'key_order') AS key_order, rk_sound('rk_col') AS sound;
ALTER TABLE rk RENAME COLUMN dim_id TO member_ref;
ALTER TABLE rk RENAME COLUMN at TO happened_at;
ALTER TABLE rk RENAME COLUMN val TO amount;
ALTER TABLE rk RENAME COLUMN note TO memo;
SELECT rk_attrs('rk_col') AS attrs, rk_opt('rk_col', 'key_order') AS key_order, rk_sound('rk_col') AS sound;
SELECT rk_redo('rk_col') AS restored_alike;
SELECT rk_sound('rk_col') AS sound;
SELECT sift_helpers.agg_equals_normal_all('SELECT member_ref, count(*), sum(amount) FROM rk WHERE happened_at >= timestamptz ''2030-01-01 12:00'' GROUP BY member_ref') AS same_read;
SELECT count(*) AS memo_hits FROM rk WHERE memo = 'n77';
INSERT INTO rk SELECT 3000 + i, 1 + i % 50, timestamptz '2030-02-01' + i * interval '1 minute', i % 7, i, 'm' || i FROM generate_series(1, 500) i;
UPDATE rk SET amount = amount + 1 WHERE id % 11 = 0;
DELETE FROM rk WHERE id % 13 = 0;
VACUUM rk;
SELECT rk_sound('rk_col') AS sound;
SELECT sift_helpers.agg_equals_normal_all('SELECT member_ref, count(*), sum(amount) FROM rk GROUP BY member_ref') AS same_read_after_writes;
ALTER TABLE rk RENAME COLUMN member_ref TO dim_id;
SELECT rk_attrs('rk_col') AS attrs, rk_sound('rk_col') AS sound;

-- the leaves kept under the name of a column: the paths of a document and the lengths of an array
CREATE TABLE rk_doc (id int PRIMARY KEY, doc jsonb NOT NULL, tags int[] NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO rk_doc SELECT i, jsonb_build_object('k', i % 4), ARRAY[i % 3, i % 5] FROM generate_series(1, 2000) i;
CREATE INDEX rk_doc_col ON rk_doc USING sift_col (id, doc, tags) WITH (paths = 'doc.k int4');
ANALYZE rk_doc;
SELECT rk_sound('rk_doc_col') AS sound;
ALTER TABLE rk_doc RENAME COLUMN doc TO document;
ALTER TABLE rk_doc RENAME COLUMN tags TO labels;
SELECT rk_opt('rk_doc_col', 'paths') AS paths, rk_sound('rk_doc_col') AS sound;
SELECT sift_helpers.agg_equals_normal_all('SELECT document -> ''k'' AS k, count(*) FROM rk_doc GROUP BY 1') AS same_paths_read;
SELECT count(*) AS label_hits FROM rk_doc WHERE labels = ARRAY[1, 2];
INSERT INTO rk_doc SELECT 2000 + i, jsonb_build_object('k', 7), ARRAY[1, 2] FROM generate_series(1, 100) i;
VACUUM rk_doc;
SELECT rk_sound('rk_doc_col') AS sound;

-- connect: the column of the child index and the key of the parent
CREATE TABLE rk_dim (id int PRIMARY KEY, grp int NOT NULL, label text) WITH (autovacuum_enabled = off);
INSERT INTO rk_dim SELECT i, i % 7, 'l' || i FROM generate_series(1, 300) i;
CREATE INDEX rk_dim_col ON rk_dim USING sift_col (id, grp, label);
CREATE TABLE rk_fact (id int PRIMARY KEY, dim_id int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO rk_fact SELECT i, 1 + i % 300, i FROM generate_series(1, 3000) i;
CREATE INDEX rk_fact_col ON rk_fact USING sift_col (id, dim_id, amount) WITH (connect = 'dim_id -> rk_dim(id)');
ANALYZE rk_dim, rk_fact;
SELECT rk_opt('rk_fact_col', 'connect') AS connect, rk_sound('rk_fact_col') AS sound;
ALTER TABLE rk_fact RENAME COLUMN dim_id TO dim_ref;
SELECT rk_opt('rk_fact_col', 'connect') AS child_column_renamed, rk_sound('rk_fact_col') AS sound;
ALTER TABLE rk_dim RENAME COLUMN id TO dim_pk;
SELECT rk_opt('rk_fact_col', 'connect') AS parent_key_renamed, rk_sound('rk_fact_col') AS sound, rk_sound('rk_dim_col') AS parent_sound;
SELECT rk_redo('rk_fact_col') AS restored_alike;
SELECT sift_helpers.agg_equals_normal_all('SELECT d.grp, count(*), sum(f.amount) FROM rk_fact f JOIN rk_dim d ON d.dim_pk = f.dim_ref GROUP BY d.grp') AS same_join;

-- scope: the column of the index, the pairs relation and its columns
SET client_min_messages = warning;
CREATE TABLE rk_pairs (member_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (member_id, tenant_id)) WITH (autovacuum_enabled = off);
INSERT INTO rk_pairs SELECT i, 1 + i % 5 FROM generate_series(1, 400) i;
CREATE INDEX rk_pairs_journal ON rk_pairs USING sift_col (member_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE rk_events (id int PRIMARY KEY, member_id int NOT NULL, created_at timestamptz NOT NULL, kind int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO rk_events SELECT i, 1 + i % 400, timestamptz '2030-01-01' + i * interval '1 minute', i % 7, i FROM generate_series(1, 3000) i;
CREATE INDEX rk_events_scope ON rk_events USING sift_col (id, member_id, created_at, kind, value_num)
	WITH (scope = 'tenant_id: member_id -> rk_pairs(member_id, tenant_id) using rk_pairs_pkey cols(kind, value_num)');
ANALYZE rk_pairs, rk_events;
SELECT rk_opt('rk_events_scope', 'scope') AS scope, rk_sound('rk_events_scope') AS sound;
ALTER TABLE rk_events RENAME COLUMN member_id TO member_ref;
ALTER TABLE rk_events RENAME COLUMN kind TO category;
SELECT rk_opt('rk_events_scope', 'scope') AS fact_columns_renamed, rk_sound('rk_events_scope') AS sound;
ALTER TABLE rk_pairs RENAME COLUMN member_id TO pair_member;
ALTER TABLE rk_pairs RENAME COLUMN tenant_id TO pair_scope;
SELECT rk_opt('rk_events_scope', 'scope') AS pair_columns_renamed, rk_sound('rk_events_scope') AS sound;
SELECT rk_redo('rk_events_scope') AS restored_alike;
SELECT sift_helpers.agg_equals_normal_all('SELECT category, count(*), sum(value_num) FROM rk_events WHERE member_ref IN (SELECT pair_member FROM rk_pairs WHERE pair_scope = 2) GROUP BY category') AS same_scope_read;

DROP TABLE rk, rk_doc, rk_fact, rk_dim, rk_events, rk_pairs;
DROP FUNCTION rk_opt(regclass, text), rk_attrs(regclass), rk_sound(regclass), rk_redo(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
