-- Calls with a NULL argument, a relation that is gone, a relation of another kind and a partitioned index return or raise an error with its own SQLSTATE: the sweep
-- of the SQL surface (test/fuzz/surface_sweep.py) found each of these ending the backend with a segmentation fault or raising XX000.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
CREATE TABLE sna_t (id int PRIMARY KEY, g int, doc jsonb) USING sift_table;
CREATE TABLE sna_heap (id int, g int);
CREATE TABLE sna_part (id int, g int) PARTITION BY RANGE (id);
CREATE TABLE sna_part_1 PARTITION OF sna_part FOR VALUES FROM (0) TO (100);
INSERT INTO sna_part SELECT i, i % 3 FROM generate_series(1, 30) i;
SET client_min_messages = warning;
CREATE INDEX sna_part_col ON sna_part USING sift_col (id, g);
RESET client_min_messages;
CREATE TABLE sna_idx_t (id int, g int);
INSERT INTO sna_idx_t SELECT i, i % 3 FROM generate_series(1, 30) i;
SET client_min_messages = warning;
CREATE INDEX sna_idx ON sna_idx_t USING sift_col (id, g);
RESET client_min_messages;
CREATE TABLE sna_gone (id int);
CREATE TABLE sna_oids AS SELECT 'sna_gone'::regclass::oid AS gone;
DROP TABLE sna_gone;

CREATE FUNCTION sna_state(call text) RETURNS text LANGUAGE plpgsql AS $$ BEGIN
	EXECUTE call;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, '[0-9]{3,}', 'N');
END $$;

SELECT call, sna_state(call) FROM (VALUES
	('SELECT sift.suggest_paths(NULL, NULL, NULL)'),
	('SELECT sift.suggest_paths(''sna_t''::regclass, NULL, 3)'),
	('SELECT sift.table_paths_check(''sna_t''::regclass, NULL, ''doc'')'),
	('SELECT sift.table_paths_check(NULL, ''doc'', NULL)'),
	('SELECT sift.table_set_paths(''sna_t''::regclass, NULL, ''doc'')'),
	('SELECT sift.table_option_values_trg()'),
	('SELECT sift._names_expr(''id'', NULL, ''x'')'),
	('SELECT sift._names_expr(''id'', ''id'', NULL)'),
	('SELECT sift._names_exprs(NULL, ''id'', ''x'')'),
	('SELECT sift._names_exprs(ARRAY[''id''], NULL, ''x'')'),
	('SELECT sift._names_option(NULL, ''id'', ''id'', ''x'')'),
	('SELECT sift._names_option(''key_cut'', ''id'', ''id'', NULL)'),
	('SELECT sift._names_expr(''1; DROP TABLE sna_t'', ''id'', ''x'')'),
	('SELECT sift.suggest_paths((SELECT gone FROM sna_oids)::regclass, ''doc'', 3)'),
	('SELECT * FROM sift.segments((SELECT gone FROM sna_oids)::regclass)'),
	('SELECT * FROM sift.tokenize(''x'', 0::oid::regclass)'),
	('SELECT sift.table_verify(0::oid::regclass)'),
	('SELECT sift.index_info(0::oid::regclass)'),
	('SELECT * FROM sift.index_choices(''sna_part_col''::regclass)'),
	('SELECT sift.index_format(''sna_part_col''::regclass)'),
	('SELECT sift.col_promote(''sna_part_col''::regclass)'),
	('SELECT sift.promote(''sna_part_col''::regclass)'),
	('SELECT sift.col_index_info(''sna_part_col''::regclass)'),
	('SELECT sift.col_index_info((SELECT gone FROM sna_oids)::regclass)'),
	('SELECT sift.col_verify((SELECT gone FROM sna_oids)::regclass)'),
	('SELECT sift.table_space(0::oid::regclass)'),
	('SELECT sift.table_layouts((SELECT gone FROM sna_oids)::regclass)'),
	('SELECT sift.table_set_paths(0::oid::regclass, ''doc'', NULL)'),
	('SELECT sift.clear_tier_policy(0::oid::regclass)'),
	('SELECT sift.drop_parts_before(0::oid::regclass, now())'),
	('SELECT sift.set_ttl(0::oid::regclass, ''ts'', ''1 day'')'),
	('SELECT sift.upgrade_table(0::oid::regclass)'),
	('SELECT sift.index_format((SELECT gone FROM sna_oids)::regclass)'),
	('SELECT sift.index_format(''sna_heap''::regclass)'),
	('SELECT sift._require_owner((SELECT gone FROM sna_oids)::regclass)'),
	('SELECT count(*) FROM sift.col_unit_hist(''sna_idx''::regclass, NULL, ''id'')'),
	('SELECT count(*) FROM sift.col_unit_hist(NULL, ''id'')'),
	('SELECT count(*) FROM sift.col_unit_hist(''sna_idx''::regclass, ''g'', NULL)'),
	('SELECT count(*) FROM sift.col_unit_agg(''sna_idx''::regclass, NULL)'),
	('SELECT count(*) FROM sift.col_unit_agg(NULL, ''g'')'),
	('SELECT count(*) FROM sift._test_col_tids(''sna_idx''::regclass, ''g'')'),
	('SELECT count(*) FROM sift._test_col_tids(''sna_idx''::regclass, ''9:eq:1'')'),
	('SELECT count(*) FROM sift._test_col_tids(''sna_idx''::regclass, ''0:bogus:1'')')
) v(call) ORDER BY call;

DROP FUNCTION sna_state(text);
DROP TABLE sna_oids, sna_part, sna_heap, sna_idx_t, sna_t;
DROP EXTENSION pg_sift_test, pg_sift;
