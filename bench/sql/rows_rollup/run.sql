-- The rollup of the rows of a unit (reloption rollup_dims, docs/COL-PREAGG.md section 2): the size it adds to the index, the build cost and three queries as the
-- normal plan, the scan of every unit (sift.agg_preagg off) and the answer from the cells (median ms, server side; the same index, only the settings differ).
-- psql db -v n=200000 -f ../../jsonb/rollup/lib.sql is read here; run gen.sql first. Variants: -v dims='kind, sub' (empty: no rollup_dims).
--   q_pair   GROUP BY two columns          q_bucket  a time bucket and a column          q_test   a test of one column, a group by another
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
\ir ../jsonb/rollup/lib.sql
\if :{?builds} \else \set builds 3 \endif
\if :{?dims} \else \set dims '' \endif

\set q_pair 'SELECT kind, sub, count(*), sum(amount) FROM facts GROUP BY 1, 2'
\set q_bucket 'SELECT mon, kind, count(*), sum(amount), max(qty) FROM facts GROUP BY 1, 2'
\set q_test 'SELECT sub, count(*), sum(amount) FROM facts WHERE kind = ''k3'' GROUP BY 1'
SELECT CASE WHEN :'dims' = '' THEN '' ELSE format(', rollup_dims = %L', :'dims') END AS opts \gset
SELECT format('CREATE INDEX docs_col ON facts USING sift_col (id, kind, sub, flag, mid_a, mid_b, mon, amount, qty, score)%s',
              CASE WHEN :'opts' = '' THEN '' ELSE ' WITH (' || substr(:'opts', 3) || ')' END) AS ddl \gset

\echo == dims=:dims n=:n
SELECT 'build serial cpu ms: ' || median_of(array_agg(build_cpu_ms(:'ddl'))) FROM generate_series(1, :builds);
SELECT build_ms(:'ddl', 0) \gset
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) facts;
SELECT 'index bytes: ' || pg_relation_size('docs_col');
SELECT 'info: ' || i::text FROM sift.col_index_info('docs_col') i;
SELECT 'pair normal/scanned/pre ms: ' || bench_ms(:'q_pair', 7, 'off', 'off') || ' / ' || bench_ms(:'q_pair', 21, 'on', 'off') || ' / ' || bench_ms(:'q_pair', 21, 'on', 'on');
SELECT 'bucket normal/scanned/pre ms: ' || bench_ms(:'q_bucket', 7, 'off', 'off') || ' / ' || bench_ms(:'q_bucket', 21, 'on', 'off') || ' / ' || bench_ms(:'q_bucket', 21, 'on', 'on');
SELECT 'test normal/scanned/pre ms: ' || bench_ms(:'q_test', 7, 'off', 'off') || ' / ' || bench_ms(:'q_test', 21, 'on', 'off') || ' / ' || bench_ms(:'q_test', 21, 'on', 'on');
