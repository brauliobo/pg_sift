-- The tables of gen.sql as sift_table (scripts/plangate.sh --table): the sift_col indexes dropped, every table and partition rewritten into the table
-- access method, the typed jsonb paths of the heap index declared on the table (the segments written before hold none: the table is rewritten), statistics and freeze as after gen.sql.
\set ON_ERROR_STOP on
DROP INDEX events_col, facts_col;
SELECT format('DROP INDEX %s', i.indexrelid::regclass) FROM pg_index i JOIN pg_class x ON x.oid = i.indexrelid JOIN pg_am a ON a.oid = x.relam
	WHERE a.amname = 'sift_col' AND i.indrelid::regclass::text LIKE 'events_part%' \gexec
SELECT format('ALTER TABLE %s SET ACCESS METHOD sift_table', c.oid::regclass) FROM pg_class c
	WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p') ORDER BY c.relname \gexec
SELECT sift.table_set_paths('events', 'doc', 'doc.metrics.* float8, doc.parts[]{kind astext, sub astext, dir_a astext, score float8}');
VACUUM FULL events;
VACUUM (FREEZE, ANALYZE) dims, events, facts, buckets, events_part;
