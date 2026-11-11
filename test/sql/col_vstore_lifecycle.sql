-- The relation of a shared value store (docs/COL-VSTORE.md): one for each (table, column) whose values several sift_col indexes share.
-- It is made by the build of the first index that uses it (sift.test_vstore_columns names the columns in this test, whatever the rule says), found by the
-- next one, dropped with the last one or with the table, and it is a materialized view that is never populated (no publication lists it): nothing reads or writes it but sift.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_vstore_columns = 'uid,url';

CREATE TABLE vs_t(id int PRIMARY KEY, tenant int, uid uuid, url text);
INSERT INTO vs_t SELECT i, i % 7, ('00000000-0000-0000-0000-' || lpad(i::text, 12, '0'))::uuid, md5(i::text) FROM generate_series(1, 500) i;
CREATE INDEX vs_a ON vs_t USING sift_col (id, tenant, uid, url);
CREATE INDEX vs_b ON vs_t USING sift_col (id, uid, url);

-- one store for each shared column, used by both indexes, with the comment that says what it is
SELECT store, table_name, column_name, indexes_using FROM sift.col_value_store_status WHERE table_name = 'vs_t'::regclass ORDER BY 1;
SELECT left(obj_description('vs_t_uid_sift_values'::regclass), 63) AS comment;
SELECT c.relname, a.amname, c.relkind FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname LIKE 'vs\_t\_%\_sift\_values' ORDER BY 1;

-- it has no rows and takes none; it cannot be indexed, truncated or refreshed; what a rewrite copies is its pages
SELECT count(*) FROM vs_t_uid_sift_values;
INSERT INTO vs_t_uid_sift_values VALUES ('x');
CREATE INDEX ON vs_t_uid_sift_values (uid);
TRUNCATE vs_t_uid_sift_values;
DO $$ BEGIN REFRESH MATERIALIZED VIEW vs_t_uid_sift_values; EXCEPTION WHEN OTHERS THEN RAISE WARNING 'refresh refused: %', sqlstate; END $$;
SELECT pg_relation_size('vs_t_uid_sift_values') AS before_rewrite \gset
VACUUM FULL vs_t_uid_sift_values;
SELECT pg_relation_size('vs_t_uid_sift_values') = :before_rewrite AS rewrite_keeps_the_pages;
ANALYZE vs_t_uid_sift_values;
VACUUM vs_t_uid_sift_values;

-- it cannot be dropped under an index; it goes with the last index
DROP MATERIALIZED VIEW vs_t_uid_sift_values;
DROP INDEX vs_a;
SELECT count(*) AS vstores FROM sift.col_value_store_status WHERE table_name = 'vs_t'::regclass;
REINDEX TABLE vs_t;
CREATE INDEX CONCURRENTLY vs_c ON vs_t USING sift_col (id, uid);
REINDEX INDEX CONCURRENTLY vs_b;
ALTER TABLE vs_t ALTER COLUMN url TYPE varchar(40);
SELECT store, column_name, indexes_using FROM sift.col_value_store_status WHERE table_name = 'vs_t'::regclass ORDER BY 1;
DROP INDEX vs_b, vs_c;
SELECT count(*) AS vstores FROM sift.col_value_store_status WHERE table_name = 'vs_t'::regclass;

-- a build after the last drop makes a new one, and DROP TABLE takes it
CREATE INDEX vs_d ON vs_t USING sift_col (id, uid);
SELECT count(*) AS vstores FROM sift.col_value_store_status WHERE table_name = 'vs_t'::regclass;
DROP TABLE vs_t;
SELECT count(*) AS vstores FROM sift.col_value_store_status WHERE store::text LIKE 'vs\_t\_%';

-- the roles: the owner of the table owns the vstores (the creator of the index may be another role that can create it) and follows ALTER TABLE OWNER;
-- nobody else can read one; the store holds no row security of its own and no row data
CREATE ROLE vs_owner1 NOLOGIN;
CREATE ROLE vs_owner2 NOLOGIN;
CREATE ROLE vs_reader NOLOGIN;
CREATE SCHEMA vs_app AUTHORIZATION vs_owner1;
CREATE TABLE vs_app.t(id int PRIMARY KEY, tenant int, uid uuid, url text);
ALTER TABLE vs_app.t OWNER TO vs_owner1;
INSERT INTO vs_app.t SELECT i, i % 3, ('00000000-0000-0000-0000-' || lpad(i::text, 12, '0'))::uuid, md5(i::text) FROM generate_series(1, 100) i;
ALTER TABLE vs_app.t ENABLE ROW LEVEL SECURITY;
CREATE POLICY p ON vs_app.t USING (tenant = 1);
GRANT USAGE ON SCHEMA vs_app TO vs_reader;
GRANT SELECT ON vs_app.t TO vs_reader;
CREATE INDEX t_a ON vs_app.t USING sift_col (id, tenant, uid, url);
CREATE INDEX t_b ON vs_app.t USING sift_col (id, uid, url);
SELECT c.relname, pg_get_userbyid(c.relowner) AS owner, c.relrowsecurity FROM pg_class c WHERE c.relname LIKE 't\_%\_sift\_values' ORDER BY 1;
ALTER TABLE vs_app.t OWNER TO vs_owner2;
SELECT c.relname, pg_get_userbyid(c.relowner) AS owner FROM pg_class c WHERE c.relname LIKE 't\_%\_sift\_values' ORDER BY 1;
SELECT has_table_privilege('vs_reader', 'vs_app.t_uid_sift_values', 'select') AS reader_reads_store,
       has_table_privilege('vs_owner2', 'vs_app.t_uid_sift_values', 'select') AS owner_reads_store;
SET ROLE vs_reader;
SELECT count(*) FROM vs_app.t_uid_sift_values;
RESET ROLE;

-- names: a schema that needs quoting, identifiers cut at 63 bytes, two columns whose names cut to the same, a partition for each partitioned leaf
CREATE SCHEMA "Vs Odd";
CREATE TABLE "Vs Odd".longtable_with_a_very_long_name_that_goes_past_limits(id int PRIMARY KEY,
  a_column_name_that_is_also_extremely_long_for_testing text, a_column_name_that_is_also_extremely_long_for_testing_2 text);
SET sift.test_vstore_columns = 'a_column_name_that_is_also_extremely_long_for_testing,a_column_name_that_is_also_extremely_long_for_testing_2';
CREATE INDEX ON "Vs Odd".longtable_with_a_very_long_name_that_goes_past_limits USING sift_col
  (id, a_column_name_that_is_also_extremely_long_for_testing, a_column_name_that_is_also_extremely_long_for_testing_2);
SELECT relnamespace::regnamespace AS schema, relname, length(relname) AS len FROM pg_class
 WHERE relname LIKE '%sift\_values%' AND relnamespace = '"Vs Odd"'::regnamespace ORDER BY 2;
SELECT column_name FROM sift.col_value_store_status WHERE table_name::text LIKE '"Vs Odd".%' ORDER BY store::text;
SET sift.test_vstore_columns = 'url';
CREATE TABLE vs_p(id int, tenant int, url text) PARTITION BY HASH (id);
CREATE TABLE vs_p0 PARTITION OF vs_p FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE vs_p1 PARTITION OF vs_p FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO vs_p SELECT i, i % 5, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX vs_pi ON vs_p USING sift_col (id, tenant, url);
CREATE INDEX vs_pj ON vs_p USING sift_col (id, url);
SELECT store, table_name, indexes_using FROM sift.col_value_store_status WHERE table_name::text LIKE 'vs\_p%' ORDER BY 1;

-- a publication of all tables does not list the vstores (they are not tables), so a subscriber that lacks them is not broken by an index
SET client_min_messages = error;
CREATE PUBLICATION vs_pub FOR ALL TABLES;
SET client_min_messages = warning;
INSERT INTO vs_p VALUES (1000, 1, 'x');
SELECT count(*) FROM pg_publication_tables WHERE pubname = 'vs_pub' AND tablename LIKE 'vs\_p%\_sift\_values';
DROP PUBLICATION vs_pub;

-- the vstores of a dropped table and schema go with them
DROP TABLE vs_p;
DROP TABLE vs_app.t;
DROP TABLE "Vs Odd".longtable_with_a_very_long_name_that_goes_past_limits;
SELECT count(*) AS stores_left FROM sift.col_value_store_status;
DROP SCHEMA vs_app;
DROP SCHEMA "Vs Odd";
DROP ROLE vs_owner1, vs_owner2, vs_reader;
DROP EXTENSION pg_sift;
