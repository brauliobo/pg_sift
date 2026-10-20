-- BM25 index size and build cost, default and positions = off, and what two queries cost on each (steps/extras.sh bm25size).
--   psql -v tbl=titles -v col=title -f bm25size.sql
-- Output: lines `ROW case arm metric value note`, tab separated; case is the table, arm the variant.
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
\pset fieldsep '\t'
\ir lib.sql
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SELECT CASE :'tbl' WHEN 'titles' THEN 'wireless AND premium' ELSE 'history AND world' END AS terms \gset

SELECT * FROM build_index(format('CREATE INDEX bm25_default ON %I USING sift (%I)', :'tbl', :'col')) \gset d_
SELECT 'ROW', :'tbl', 'default', 'index_bytes', pg_relation_size('bm25_default'), '';
SELECT 'ROW', :'tbl', 'default', 'build_cpu_ms', :d_cpu_ms, '';
SELECT 'ROW', :'tbl', 'default', 'count_ms', timed_ms(format('SELECT id FROM %I WHERE %I @@> %L', :'tbl', :'col', :'terms'), 21, 'enable_seqscan=off'), :'terms';
SELECT 'ROW', :'tbl', 'default', 'top10_ms', timed_ms(format('SELECT id FROM %I WHERE %I @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10', :'tbl', :'col', :'terms'), 21, 'enable_seqscan=off'), :'terms';
DROP INDEX bm25_default;

SELECT * FROM build_index(format('CREATE INDEX bm25_nopos ON %I USING sift (%I) WITH (positions = off)', :'tbl', :'col')) \gset n_
SELECT 'ROW', :'tbl', 'positions_off', 'index_bytes', pg_relation_size('bm25_nopos'), '';
SELECT 'ROW', :'tbl', 'positions_off', 'build_cpu_ms', :n_cpu_ms, '';
SELECT 'ROW', :'tbl', 'positions_off', 'count_ms', timed_ms(format('SELECT id FROM %I WHERE %I @@> %L', :'tbl', :'col', :'terms'), 21, 'enable_seqscan=off'), :'terms';
SELECT 'ROW', :'tbl', 'positions_off', 'top10_ms', timed_ms(format('SELECT id FROM %I WHERE %I @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10', :'tbl', :'col', :'terms'), 21, 'enable_seqscan=off'), :'terms';
DROP INDEX bm25_nopos;
