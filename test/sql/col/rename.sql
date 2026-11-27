-- What a sift_col index and sift.scope_follows keep under the name of a column or a table follows the name (src/col/names.c). The options of an index name its columns as text (key_cut,
-- key_columns, rollup_columns, track_changes ...), which pg_dump writes and a restore reads again: a column renamed under them made a dump that could not restore its index. The renames
-- here are checked as the dump and the restore see them: the definition of the index (pg_get_indexdef, with its options) is run again on the table as it is. A column whose leaves the
-- segments keep under its name (an array with lengths, a jsonb with paths) is refused. The pairs a scope follows are kept by the oid of their tables: a rename or a move of the pair
-- table or of the source table, and of the column of the source, does not stop the writes to the source.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
-- Format 1: the leaf names of the metapage and of the segments this test reads are Format 1.
SET sift.test_create_format1 = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE TABLE rn (id int PRIMARY KEY, region text, cust int, created date, amount numeric(10, 2), tags int[], doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO rn SELECT g, 'r' || g % 5, g % 20, date '2025-01-01' + g % 300, g, ARRAY[g % 3], jsonb_build_object('k', g % 4) FROM generate_series(1, 3000) g;
CREATE INDEX rn_col ON rn USING sift_col (id, region, cust, created, amount) WITH (key_cut = 'created', key_columns = 'cust', rollup_columns = 'region, cust', order_array_columns = 'amount');
CREATE INDEX rn_arr ON rn USING sift_col (id, tags);
CREATE INDEX rn_json ON rn USING sift_col (id, doc) WITH (paths = 'doc.k int4');
ANALYZE rn;

CREATE FUNCTION rn_state(idx regclass) RETURNS TABLE (options text, attributes text, verified boolean) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.col_verify(idx);
	RETURN QUERY SELECT (SELECT string_agg(o, ' ' ORDER BY o) FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = idx AND o NOT LIKE 'array_lengths%'),
	                    (SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute WHERE attrelid = idx), true;
END $$;
-- the reads are written for the names the table has now: they are replaced once the columns are renamed
CREATE FUNCTION rn_reads_as(created text, cust text, amount text) RETURNS TABLE (q text, pushed boolean, same boolean) LANGUAGE sql AS $$
	SELECT c.q, sift_helpers.plan_mentions_siftagg(c.q), sift_helpers.agg_equals_normal_all(c.q) FROM (SELECT replace(replace(replace(q, 'created_', created), 'cust_', cust), 'amount_', amount) FROM
		(SELECT unnest(ARRAY[
			'SELECT region, count(*), sum(amount_) FROM rn GROUP BY region',
			'SELECT cust_, count(*), sum(amount_) FROM rn WHERE created_ >= date ''2025-06-01'' GROUP BY cust_',
			'SELECT created_, count(*) FROM rn WHERE cust_ = 3 GROUP BY created_']) q) x) AS c(q) $$;

SELECT * FROM rn_state('rn_col');
SELECT * FROM rn_reads_as('created', 'cust', 'amount');

-- the columns the options name are renamed, one statement each, as a user does
ALTER TABLE rn RENAME COLUMN created TO created_at;
ALTER TABLE rn RENAME COLUMN cust TO customer_id;
ALTER TABLE rn RENAME COLUMN amount TO total;
SELECT * FROM rn_state('rn_col');
SELECT * FROM rn_reads_as('created_at', 'customer_id', 'total');

-- what a restore does: the definition of the index, as pg_dump writes it, run on the table as it is now
SELECT pg_get_indexdef('rn_col'::regclass) AS def \gset
SELECT options AS before_options FROM rn_state('rn_col') \gset
DROP INDEX rn_col;
:def;
SELECT options = :'before_options' AS restored_with_the_same_options, verified FROM rn_state('rn_col');
SELECT * FROM rn_reads_as('created_at', 'customer_id', 'total');

-- the table is written and the index maintained under the new names
INSERT INTO rn SELECT 3000 + g, 'r1', g % 20, date '2025-12-01', g, ARRAY[1], '{"k": 1}' FROM generate_series(1, 200) g;
UPDATE rn SET total = total + 1 WHERE id % 7 = 0;
DELETE FROM rn WHERE id % 11 = 0;
SELECT sift.col_promote('rn_col') >= 0 AS promoted;
SELECT * FROM rn_reads_as('created_at', 'customer_id', 'total');
REINDEX INDEX rn_col;
SELECT * FROM rn_state('rn_col');

-- a rename back, and a rename of the primary key the track_changes option names
ALTER TABLE rn RENAME COLUMN customer_id TO cust;
ALTER TABLE rn RENAME COLUMN id TO rn_id;
SELECT * FROM rn_state('rn_col');
ALTER TABLE rn RENAME COLUMN rn_id TO id;

-- the leaves of these are kept under the name of their column (the lengths of an array, the paths of a document): the metapage takes the names the index has now and the segments keep the
-- ones they were written with, so the rename needs no rebuild, and a segment written after it merges with the older ones
CREATE FUNCTION rn_leaves(idx regclass) RETURNS text LANGUAGE sql AS $$ SELECT string_agg(path, ',' ORDER BY leaf) FROM sift.col_leaves(idx) WHERE path <> '' $$;
SELECT rn_leaves('rn_arr') AS arr_leaves, rn_leaves('rn_json') AS json_leaves;
ALTER TABLE rn RENAME COLUMN tags TO labels;
ALTER TABLE rn RENAME COLUMN doc TO document;
SELECT attname FROM pg_attribute WHERE attrelid = 'rn'::regclass AND attnum > 0 AND NOT attisdropped ORDER BY attnum;
SELECT rn_leaves('rn_arr') AS arr_leaves, rn_leaves('rn_json') AS json_leaves;
SELECT * FROM rn_state('rn_arr');
SELECT * FROM rn_state('rn_json');
SELECT count(*) FROM rn WHERE labels = ARRAY[1];
SELECT sift_helpers.agg_equals_normal_all('SELECT document -> ''k'' AS k, count(*) FROM rn GROUP BY 1') AS same_paths_read;
INSERT INTO rn SELECT 4000 + g, 'r2', g % 20, date '2025-12-01', g, ARRAY[2], jsonb_build_object('k', 7) FROM generate_series(1, 100) g;
SELECT sift.col_promote('rn_arr') >= 0 AS arr_promoted, sift.col_promote('rn_json') >= 0 AS json_promoted;
SELECT count(*) AS arr_segments FROM sift.col_segments('rn_arr') WHERE kind = 'immutable';
SELECT sift.col_merge('rn_arr', true) >= 0 AS arr_merged, sift.col_merge('rn_json', true) >= 0 AS json_merged;
SELECT count(*) AS arr_segments FROM sift.col_segments('rn_arr') WHERE kind = 'immutable';
SELECT * FROM rn_state('rn_arr');
SELECT * FROM rn_state('rn_json');
SELECT sift_helpers.agg_equals_normal_all('SELECT document -> ''k'' AS k, count(*) FROM rn GROUP BY 1') AS same_paths_read_after_merge;
SELECT count(*) FROM rn WHERE labels = ARRAY[2];
SELECT rn_leaves('rn_arr') AS arr_leaves, rn_leaves('rn_json') AS json_leaves;
REINDEX INDEX rn_arr;
SELECT * FROM rn_state('rn_arr');

-- a column of a partitioned table: the index of the partition follows too
CREATE TABLE rp (id int, grp int, created date, v int) PARTITION BY RANGE (id);
CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (0) TO (1000);
CREATE TABLE rp2 PARTITION OF rp FOR VALUES FROM (1000) TO (2000);
INSERT INTO rp SELECT g, g % 9, date '2025-01-01' + g % 100, g FROM generate_series(1, 1999) g;
CREATE INDEX rp_col ON rp USING sift_col (id, grp, created, v) WITH (key_cut = 'created', key_columns = 'grp');
ALTER TABLE rp RENAME COLUMN created TO day;
ALTER TABLE rp RENAME COLUMN grp TO bucket;
SELECT i.indrelid::regclass::text AS on_table, o AS option FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam, unnest(c.reloptions) o
 WHERE a.amname = 'sift_col' AND i.indrelid::regclass::text LIKE 'rp%' AND o ~ '^key_' ORDER BY 1, 2;
SELECT sift_helpers.agg_equals_normal_all('SELECT bucket, count(*), sum(v) FROM rp WHERE day >= date ''2025-02-01'' GROUP BY bucket') AS partitioned_same;

-- the pairs a scope follows: the pair table, the source table and its column, and the schema of the source change their names
CREATE TABLE fl_src (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int NOT NULL) WITH (autovacuum_enabled = off);
CREATE VIEW fl_view AS SELECT DISTINCT dim_a_id, tenant_id FROM fl_src;
CREATE TABLE fl_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id)) WITH (autovacuum_enabled = off);
INSERT INTO fl_src (tenant_id, dim_a_id) SELECT 1 + g % 4, g FROM generate_series(1, 40) g;
SELECT inserted, deleted FROM sift.scope_follow_pairs('fl_pairs', 'fl_view', 'fl_src', 'key', 'tenant_id');
CREATE FUNCTION fl_equal(pairs regclass, view regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((TABLE %1$s EXCEPT TABLE %2$s) UNION ALL (TABLE %2$s EXCEPT TABLE %1$s)) d', pairs, view) INTO n;
	RETURN n = 0;
END $$;
SELECT fl_equal('fl_pairs', 'fl_view') AS equal_to_view;
ALTER TABLE fl_pairs RENAME TO fl_pairs2;
INSERT INTO fl_src (tenant_id, dim_a_id) VALUES (9, 90);
SELECT fl_equal('fl_pairs2', 'fl_view') AS after_the_pair_table_was_renamed, (SELECT count(*) FROM fl_pairs2 WHERE tenant_id = 9) AS new_pair;
ALTER TABLE fl_src RENAME TO fl_src2;
INSERT INTO fl_src2 (tenant_id, dim_a_id) VALUES (9, 91);
SELECT fl_equal('fl_pairs2', 'fl_view') AS after_the_source_was_renamed, (SELECT count(*) FROM fl_pairs2 WHERE tenant_id = 9) AS new_pairs;
ALTER TABLE fl_src2 RENAME COLUMN tenant_id TO tenant;
SELECT source, source_column FROM sift.scope_follows;
INSERT INTO fl_src2 (tenant, dim_a_id) VALUES (10, 92);
DELETE FROM fl_src2 WHERE tenant = 9;
SELECT fl_equal('fl_pairs2', 'fl_view') AS after_the_column_was_renamed, (SELECT count(*) FROM fl_pairs2 WHERE tenant_id = 9) AS gone_pairs;
CREATE SCHEMA fl_s;
ALTER TABLE fl_src2 SET SCHEMA fl_s;
INSERT INTO fl_s.fl_src2 (tenant, dim_a_id) VALUES (11, 93);
SELECT fl_equal('fl_pairs2', 'fl_view') AS after_the_source_moved, (SELECT count(*) FROM fl_pairs2 WHERE tenant_id = 11) AS moved_pair;
SELECT source, pairs, definition FROM sift.scope_follows;
SELECT sift.scope_unfollow_pairs('fl_pairs2');
SELECT count(*) AS follows FROM sift.scope_follows;
SELECT count(*) AS triggers_left FROM pg_trigger WHERE tgname LIKE 'sift\_scope\_%';

-- the owner of a table, who is no superuser, renames a column too (the trigger writes what the owner has no right to)
CREATE ROLE rn_owner;
GRANT CREATE ON SCHEMA public TO rn_owner;
CREATE TABLE rn_own (id int PRIMARY KEY, cust int, created date) WITH (autovacuum_enabled = off);
INSERT INTO rn_own SELECT g, g % 5, date '2025-01-01' + g FROM generate_series(1, 500) g;
CREATE INDEX rn_own_col ON rn_own USING sift_col (id, cust, created) WITH (key_columns = 'cust', key_cut = 'created');
ALTER TABLE rn_own OWNER TO rn_owner;
SET ROLE rn_owner;
ALTER TABLE rn_own RENAME COLUMN cust TO customer_id;
ALTER TABLE rn_own RENAME TO rn_owned;
RESET ROLE;
CREATE TABLE rn_osrc (id serial PRIMARY KEY, member int NOT NULL, tenant int NOT NULL);
CREATE TABLE rn_opairs (member int NOT NULL, tenant int NOT NULL, PRIMARY KEY (tenant, member));
CREATE VIEW rn_oview AS SELECT DISTINCT member, tenant FROM rn_osrc;
ALTER TABLE rn_osrc OWNER TO rn_owner;
ALTER TABLE rn_opairs OWNER TO rn_owner;
ALTER VIEW rn_oview OWNER TO rn_owner;
GRANT USAGE ON SCHEMA sift TO rn_owner;
SET ROLE rn_owner;
SELECT inserted FROM sift.scope_follow_pairs('rn_opairs', 'rn_oview', 'rn_osrc', 'key', 'tenant');
ALTER TABLE rn_osrc RENAME COLUMN tenant TO tenant_id;
ALTER TABLE rn_osrc RENAME TO rn_osrc2;
INSERT INTO rn_osrc2 (member, tenant_id) VALUES (5, 1), (6, 2);
SELECT count(*) AS owner_pairs FROM rn_opairs;
RESET ROLE;
SELECT source, source_column FROM sift.scope_follows;
SET ROLE rn_owner;
SELECT sift.scope_unfollow_pairs('rn_opairs');
RESET ROLE;
SELECT * FROM rn_state('rn_own_col');
DROP VIEW rn_oview;
DROP TABLE rn_osrc2, rn_opairs;
REVOKE USAGE ON SCHEMA sift FROM rn_owner;
DROP TABLE rn_owned;
REVOKE CREATE ON SCHEMA public FROM rn_owner;
DROP ROLE rn_owner;

-- a rename the segments cannot follow says so and names the replacement. DROP NOT NULL on one of two jsonb columns leaves the index with no layout that matches its segments (a read already
-- asks for the REINDEX); a rename of a healthy index says nothing
CREATE TABLE rn_stale (id int PRIMARY KEY, a jsonb, b jsonb NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO rn_stale SELECT g, jsonb_build_object('k', g), jsonb_build_object('k', g) FROM generate_series(1, 200) g;
CREATE INDEX rn_stale_col ON rn_stale USING sift_col (id, a, b);
SET client_min_messages = notice;
ALTER TABLE rn_stale RENAME COLUMN b TO b2;
SET client_min_messages = warning;
ALTER TABLE rn_stale ALTER COLUMN b2 DROP NOT NULL;
SET client_min_messages = notice;
ALTER TABLE rn_stale RENAME COLUMN a TO a2;
SET client_min_messages = warning;
CREATE INDEX CONCURRENTLY rn_stale_new ON rn_stale USING sift_col (id, a2, b2);
DROP INDEX rn_stale_col;
SELECT * FROM rn_state('rn_stale_new');
SET client_min_messages = notice;
ALTER TABLE rn_stale RENAME COLUMN a2 TO a3;
SET client_min_messages = warning;
SELECT * FROM rn_state('rn_stale_new');
DROP TABLE rn_stale;

DROP TABLE fl_pairs2, rp, rn;
DROP VIEW fl_view;
DROP TABLE fl_s.fl_src2;
DROP SCHEMA fl_s;
DROP FUNCTION fl_equal(regclass, regclass), rn_state(regclass), rn_reads_as(text, text, text), rn_leaves(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
