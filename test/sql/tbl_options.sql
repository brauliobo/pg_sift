-- Per-table options of a sift_table (sift.table_set_option and its companions): the value is checked as the setting of the same name is, the table's option
-- beats the setting whatever its source, and the rows follow the table through RENAME, SET SCHEMA, rewrites and DROP.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE ROLE opt_owner LOGIN;
CREATE ROLE opt_reader LOGIN;
CREATE ROLE opt_other LOGIN;
GRANT ALL ON SCHEMA public TO PUBLIC;
RESET client_min_messages;

SET ROLE opt_owner;
CREATE TABLE ot (id int, v text) USING sift_table;
CREATE TABLE ot2 (id int, v text) USING sift_table;
CREATE TABLE heap_t (id int);
GRANT SELECT ON ot TO opt_reader;
RESET ROLE;

-- nothing is set: every option reads the setting
SELECT count(*) AS options FROM sift.table_options('ot');
SELECT name, value, source, default_value FROM sift.table_options('ot') WHERE name IN ('write_buffer_limit', 'compression', 'hot_update') ORDER BY name;
SELECT sift.table_get_option('ot', 'write_buffer_limit');
-- every function names its table argument tbl
SELECT sift.table_get_option(tbl => 'ot', name => 'hot_update');
SELECT count(*) AS options_named FROM sift.table_options(tbl => 'ot');

-- set, read back in the canonical text of the setting, reset
SET ROLE opt_owner;
SELECT sift.table_set_option('ot', 'write_buffer_limit', '2048');
SELECT sift.table_get_option('ot', 'write_buffer_limit');
SELECT sift.table_set_option('ot', 'compression', 'zstd:3');
SELECT sift.table_set_option('ot', 'hot_update', 'off');
SELECT sift.table_set_option('ot', 'hot_update', 'false');
SELECT name, value, source FROM sift.table_options('ot') WHERE source = 'table' ORDER BY name;
SELECT name, value FROM sift.table_options('ot2') WHERE name = 'write_buffer_limit';
SELECT sift.table_reset_option('ot', 'compression');
SELECT sift.table_reset_option('ot', 'compression');
SELECT sift.table_set_option(tbl => 'ot2', name => 'wide_rows', value => 'off');
SELECT sift.table_reset_option(tbl => 'ot2', name => 'wide_rows');
SELECT name, value, source FROM sift.table_options('ot') WHERE source = 'table' ORDER BY name;

-- a value the setting refuses is refused, with the reason of the setting
SELECT sift.table_set_option('ot', 'write_buffer_limit', 'abc');
SELECT sift.table_set_option('ot', 'write_buffer_limit', '1MB');
SELECT sift.table_set_option('ot', 'compression', 'bogus!');
SELECT sift.table_set_option('ot', 'hot_update', 'maybe');
SELECT sift.table_set_option('ot', 'nope', '1');
SELECT sift.table_set_option('ot', 'auto_defrag_delay', '1s');
SELECT sift.table_set_option('heap_t', 'hot_update', 'on');
SELECT sift.table_set_option('ot', 'hot_update', NULL);
SELECT sift.table_get_option('ot', 'nope');
RESET ROLE;

-- who may set and who may read
SET ROLE opt_other;
SELECT sift.table_set_option('ot', 'hot_update', 'on');
SELECT sift.table_reset_option('ot');
SELECT sift.table_get_option('ot', 'hot_update');
SELECT count(*) AS other_sees FROM sift.table_options();
RESET ROLE;
SET ROLE opt_reader;
SELECT sift.table_get_option('ot', 'hot_update');
SELECT sift.table_set_option('ot', 'hot_update', 'on');
SELECT DISTINCT rel FROM sift.table_options();
RESET ROLE;
-- the owner needs no privilege on the table that holds the rows, and nobody reads it
SET ROLE opt_owner;
SELECT count(*) FROM sift.table_option_values;
RESET ROLE;

-- precedence: the option of the table beats the setting, whatever its source; reset takes the table back to the setting
SET sift.table_write_buffer_limit = '8MB';
SELECT name, value, source FROM sift.table_options('ot2') WHERE name = 'write_buffer_limit';
SELECT sift.table_get_option('ot', 'write_buffer_limit');
SELECT sift.table_set_option('ot2', 'write_buffer_limit', '4MB');
SELECT sift.table_get_option('ot2', 'write_buffer_limit');
SELECT name, value, source FROM sift.table_options('ot2') WHERE name = 'write_buffer_limit';
SELECT sift.table_reset_option('ot2', 'write_buffer_limit');
SELECT sift.table_get_option('ot2', 'write_buffer_limit');
RESET sift.table_write_buffer_limit;

-- the option acts: a row larger than the write buffer is refused by the table that turns wide rows off, with the setting on
SELECT sift.table_set_option('ot', 'wide_rows', 'off');
INSERT INTO ot VALUES (1, repeat('a', 5000000));
INSERT INTO ot2 VALUES (1, repeat('a', 5000000));
SELECT sift.table_reset_option('ot', 'wide_rows');
INSERT INTO ot VALUES (2, repeat('b', 5000000));
SELECT 'ot' AS t, id, length(v) FROM ot UNION ALL SELECT 'ot2', id, length(v) FROM ot2 ORDER BY 1, 2;

-- the options go with the table through every rewrite and rename
SELECT sift.table_set_option('ot', 'hot_update', 'off');
SELECT sift.table_set_option('ot', 'backpressure', '60');
TRUNCATE ot;
VACUUM FULL ot;
CREATE INDEX ot_id ON ot (id);
CLUSTER ot USING ot_id;
REINDEX TABLE ot;
SELECT name, value FROM sift.table_options('ot') WHERE source = 'table' ORDER BY name;
ALTER TABLE ot RENAME TO ot_renamed;
CREATE SCHEMA other_schema;
ALTER TABLE ot_renamed SET SCHEMA other_schema;
SELECT rel, name, value FROM sift.table_options('other_schema.ot_renamed') WHERE source = 'table' ORDER BY name;
SELECT sift.table_get_option('other_schema.ot_renamed', 'hot_update');

-- the heap and the options stay apart: a table moved off the access method keeps no visible options, and comes back with them
ALTER TABLE other_schema.ot_renamed SET ACCESS METHOD heap;
SELECT count(*) AS visible FROM sift.table_options('other_schema.ot_renamed');
SELECT sift.table_get_option('other_schema.ot_renamed', 'hot_update');
ALTER TABLE other_schema.ot_renamed SET ACCESS METHOD sift_table;
SELECT name, value FROM sift.table_options('other_schema.ot_renamed') WHERE source = 'table' ORDER BY name;

-- a row written straight to the table of the options (what a restore does) acts like the function, and a table that is not there is skipped
INSERT INTO sift.table_option_values VALUES ('ot2'::regclass, 'hot_update', 'off');
SELECT sift.table_get_option('ot2', 'hot_update');
COPY sift.table_option_values (rel, name, value) FROM stdin;
public.no_such_table	hot_update	off
\.
SELECT count(*) AS rows_for_missing_table FROM sift.table_option_values WHERE rel::oid = 0;
SELECT sift.table_reset_option('ot2');

-- DROP leaves no row: the table, its schema, an owner's objects
SELECT count(*) AS rows_before FROM sift.table_option_values;
DROP TABLE other_schema.ot_renamed;
SELECT count(*) AS rows_after_drop FROM sift.table_option_values;
SELECT sift.table_set_option('ot2', 'hot_update', 'off');
CREATE TABLE other_schema.in_schema (id int) USING sift_table;
SELECT sift.table_set_option('other_schema.in_schema', 'hot_update', 'off');
DROP SCHEMA other_schema CASCADE;
SELECT rel, name FROM sift.table_option_values ORDER BY 1, 2;
DROP OWNED BY opt_owner;
SELECT count(*) AS rows_after_drop_owned FROM sift.table_option_values;

DROP EXTENSION pg_sift;
DROP ROLE opt_owner;
DROP ROLE opt_reader;
DROP ROLE opt_other;
