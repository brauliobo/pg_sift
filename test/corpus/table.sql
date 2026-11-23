-- The tables of lab.sql as sift_table (scripts/corpus.sh --table): every sift_col index dropped, every table and partition rewritten into the table access method, the typed jsonb
-- paths of the heap index declared on the table, statistics and freeze as after lab.sql. The views and the other indexes stay.
\set ON_ERROR_STOP on
SELECT format('DROP INDEX IF EXISTS %s CASCADE', i.indexrelid::regclass) FROM pg_index i JOIN pg_class x ON x.oid = i.indexrelid JOIN pg_am a ON a.oid = x.relam WHERE a.amname = 'sift_col' AND NOT x.relispartition \gexec
SELECT format('ALTER TABLE %s SET ACCESS METHOD sift_table', c.oid::regclass) FROM pg_class c
	WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p') ORDER BY c.relname \gexec
SELECT sift.table_set_paths('events', 'attrs', 'attrs.a float8, attrs.b astext, attrs.flag astext');
VACUUM FULL events;
SELECT format('VACUUM (FREEZE, ANALYZE) %s', c.oid::regclass) FROM pg_class c WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p') ORDER BY c.relname \gexec
