-- The rollups of the elements of a list (docs/COL-PREAGG.md 2, 7): CREATE INDEX time (serial CPU, wall, two workers; median and minimum of :builds), sidecar bytes,
-- and j02, j17, j17b, j17c, j17d as the normal plan, the scan of every unit (sift.agg_preagg off) and the answer from the rollups (median ms, server side).
-- createdb jb && psql jb -c 'CREATE EXTENSION pg_sift' && psql jb -v n=1000000 -f ../gen.sql
-- psql jb -v n=1000000 -v variant=default -v builds=5 -f run.sql                                    -- the automatic rule
-- psql jb -v n=1000000 -v variant=off -v builds=5 -v opts=", rollup = 'off'" -f run.sql             -- no rollups
-- psql jb -v n=1000000 -v variant=explicit -v builds=5 -v opts=", rollup = 'items[].kind, items[].sub, items[].flag'" -f run.sql
-- ondemand.sh: the time from ALTER INDEX ... SET (rollup = ...) to the first query answered from the rollups (the worker on).
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
\ir lib.sql
\if :{?builds} \else \set builds 5 \endif

\set q_j02 'SELECT e.kind, count(*), round(avg(e.score)::numeric, 6) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) GROUP BY 1'
\set q_j17 'SELECT e.kind, e.flag, count(*) AS n, round(sum(e.score)::numeric, 6) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) GROUP BY 1, 2 HAVING count(*) > 100'
\set q_j17b 'SELECT e.kind, count(*) AS n, round(sum(e.score)::numeric, 6) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) GROUP BY 1 HAVING count(*) > 100'
\set q_j17d 'SELECT e.kind, count(*), round(sum(e.score)::numeric, 6) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) WHERE e.flag = ''fa'' AND e.sub IN (''s1'', ''s2'', ''s3'') GROUP BY 1'
\set q_j17c 'SELECT e.kind, e.sub, e.flag, count(*), min(e.score), max(e.score), round(sum(e.score)::numeric, 6) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) GROUP BY 1, 2, 3'

\set paths 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, scores.* float8, items[]{kind text, sub text, flag text, score float8}'
\if :{?opts} \else \set opts '' \endif
SELECT format('CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, scores, items) WITH (paths = %L%s)', :'paths', :'opts') AS ddl_default \gset

\echo == :variant n=:n
-- builds: serial and 2 workers, median of :builds
SELECT 'build serial cpu ms: ' || median_of(array_agg(build_cpu_ms(:'ddl_default'))) FROM generate_series(1, :builds);
SELECT 'build serial ms: ' || median_of(array_agg(build_ms(:'ddl_default', 0))) FROM generate_series(1, :builds);
SELECT 'build 2 workers ms: ' || median_of(array_agg(build_ms(:'ddl_default', 2))) FROM generate_series(1, :builds);
SELECT build_ms(:'ddl_default', 0) \gset
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) docs;
SELECT 'index bytes: ' || pg_relation_size('docs_col');
SELECT 'info: ' || i::text FROM sift.col_index_info('docs_col') i;
SELECT 'j02 normal/scanned/pre ms: ' || bench_ms(:'q_j02', 7, 'off', 'off') || ' / ' || bench_ms(:'q_j02', 21, 'on', 'off') || ' / ' || bench_ms(:'q_j02', 21, 'on', 'on');
SELECT 'j17 normal/scanned/pre ms: ' || bench_ms(:'q_j17', 7, 'off', 'off') || ' / ' || bench_ms(:'q_j17', 21, 'on', 'off') || ' / ' || bench_ms(:'q_j17', 21, 'on', 'on');
SELECT 'j17b normal/scanned/pre ms: ' || bench_ms(:'q_j17b', 7, 'off', 'off') || ' / ' || bench_ms(:'q_j17b', 21, 'on', 'off') || ' / ' || bench_ms(:'q_j17b', 21, 'on', 'on');
SELECT 'j17d normal/scanned/pre ms: ' || bench_ms(:'q_j17d', 7, 'off', 'off') || ' / ' || bench_ms(:'q_j17d', 21, 'on', 'off') || ' / ' || bench_ms(:'q_j17d', 21, 'on', 'on');
SELECT 'j17c normal/scanned/pre ms: ' || bench_ms(:'q_j17c', 7, 'off', 'off') || ' / ' || bench_ms(:'q_j17c', 21, 'on', 'off') || ' / ' || bench_ms(:'q_j17c', 21, 'on', 'on');
