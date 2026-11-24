-- The twenty options that only Format 1 reads stay registered, so that dumps, upgraded indexes and ALTER INDEX .. RESET keep working: a Format 2.0 index accepts them with a
-- NOTICE and ignores them, a Format 1 index takes them without a word. sift_table says the same of its hidden options of Format 1.
CREATE EXTENSION pg_sift_test CASCADE;
\set VERBOSITY terse
CREATE TABLE rd (id int PRIMARY KEY, k int, g int, v int, t text);
INSERT INTO rd SELECT i, i % 50, i % 7, i, 'w' || i % 9 FROM generate_series(1, 2000) i;

-- Format 2.0: every Format 1 option written says so, once, on CREATE INDEX and on ALTER INDEX
SET sift.test_create_format1 = off;
CREATE INDEX rd2 ON rd USING sift_col (id, k, g, v, t) WITH (key_cut = k, superunit = 8, write_buffer_limit = 512, hll_columns = k, key_columns = g, cluster_advice = off);
SELECT major FROM sift.index_format('rd2');
SELECT reloptions FROM pg_class WHERE relname = 'rd2';
ALTER INDEX rd2 SET (cube_minmax = off);
ALTER INDEX rd2 SET (order_array_columns = 'k', rollup_columns = 'g, k', calendar_columns = off);
ALTER INDEX rd2 SET (array_lengths = off, dict_lookup = off, key_table = off, count_columns = off, point_tid_columns = 'k', return_columns = 't', rollup_on_demand = off);
ALTER INDEX rd2 SET (order_layout = whole, order_array_group = g, rollup_leaves = off);
-- the options of Format 2.0 say nothing
ALTER INDEX rd2 SET (compression = 'lz4', key_order = 'k', dead_rewrite_pct = 20);
ALTER INDEX rd2 RESET (compression, key_order, dead_rewrite_pct);
-- RESET drops them without a word
ALTER INDEX rd2 RESET (key_cut, superunit, write_buffer_limit, hll_columns, key_columns, cluster_advice, cube_minmax, order_array_columns, rollup_columns);
ALTER INDEX rd2 RESET (calendar_columns, array_lengths, dict_lookup, key_table, count_columns, point_tid_columns, return_columns, rollup_on_demand);
ALTER INDEX rd2 RESET (order_layout, order_array_group, rollup_leaves);
SELECT reloptions FROM pg_class WHERE relname = 'rd2';
SELECT sift.col_verify('rd2', true);
DROP INDEX rd2;

-- Format 1: the same options, no notice
SET sift.test_create_format1 = on;
CREATE INDEX rd1 ON rd USING sift_col (id, k, g, v, t) WITH (key_cut = k, superunit = 8, write_buffer_limit = 512, hll_columns = k, key_columns = g, cluster_advice = off);
SELECT major FROM sift.index_format('rd1');
ALTER INDEX rd1 SET (cube_minmax = off);
ALTER INDEX rd1 SET (order_array_columns = 'k', rollup_columns = 'g, k', calendar_columns = off);
ALTER INDEX rd1 SET (array_lengths = off, dict_lookup = off, key_table = off, count_columns = off, rollup_on_demand = off);
ALTER INDEX rd1 SET (order_layout = whole, order_array_group = g, rollup_leaves = off);
ALTER INDEX rd1 RESET (key_cut, superunit, write_buffer_limit, hll_columns, key_columns, cluster_advice, cube_minmax, order_array_columns, rollup_columns);
ALTER INDEX rd1 RESET (calendar_columns, array_lengths, dict_lookup, key_table, count_columns, rollup_on_demand, order_layout, order_array_group, rollup_leaves);
SELECT reloptions FROM pg_class WHERE relname = 'rd1';
DROP INDEX rd1;

-- sift_table of Format 2.0: ten options, thirteen of Format 1 accepted and ignored with a NOTICE, RESET without one
SET sift.test_create_format1 = off;
CREATE TABLE rt (id int, v text) USING sift_table;
SELECT sift.table_set_option('rt', 'compression', 'zstd:3');
SELECT sift.table_set_option('rt', 'auto_defrag', 'off');
SELECT sift.table_set_option('rt', 'dead_rewrite_pct', '20');
SELECT sift.table_set_option('rt', 'write_buffer_limit', '2048');
SELECT sift.table_set_option('rt', 'wide_rows', 'off');
SELECT sift.table_set_option('rt', 'auto_defrag_budget', '64');
SELECT name, value, source FROM sift.table_options('rt') WHERE source = 'table' ORDER BY name;
SELECT sift.table_reset_option('rt');
SELECT count(*) AS options_left FROM sift.table_options('rt') WHERE source = 'table';

DROP TABLE rt, rd;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
