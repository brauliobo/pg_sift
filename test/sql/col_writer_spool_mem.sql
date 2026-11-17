-- The memory of the entries a segment writer holds for its order arrays is bounded by its settings, not by the rows: the bytes the writer's context of them holds at its checkpoints
-- (col_writer_spool while the units come, col_writer_order while the blocks are built) are the same for 20,000 rows as for 36,000 once the runs are on their file, to a file buffer of 8 kB
-- (the sets that spilled), a build and a merge alike. eset_rows = 256 makes the runs of 256 entries; a build with no limit holds all the entries as arrays (about 1 MB more at 36,000 rows) and fails each check.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
SET sift.async_maintenance = off;
SET sift.test_limits = 'eset_rows=256';

CREATE TABLE peaks (n int, stage text, scope text, peak bigint);

SELECT s FROM unnest(ARRAY[20000, 24000, 28000, 32000, 36000]) n, LATERAL unnest(ARRAY[
	format('CREATE TABLE ws WITH (autovacuum_enabled = off) AS SELECT i AS id, CASE WHEN i %% 31 = 0 THEN NULL ELSE i * 7919 %% 10007 END AS k,
		TIMESTAMP ''2026-01-01'' + (i::bigint * 7919 %% 300011) * INTERVAL ''1 minute'' AS ts, ''s'' || lpad((i::bigint * 7919 %% 50)::text, 6, ''0'') AS sc, i %% 100 AS amount
		FROM generate_series(1, %s) i', n),
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'CREATE INDEX ws_i ON ws USING sift_col (id, k, ts, sc, amount) WITH (key_cut = ''off'', key_columns = ''amount'', order_array_columns = ''id, k, ts, sc'')',
	format('INSERT INTO peaks SELECT %s, ''build'', scope, peak FROM sift._test_mem_highwater(true) WHERE scope LIKE ''col_writer%%''', n),
	'UPDATE ws SET amount = amount + 1 WHERE id % 3 = 0',
	'VACUUM ws',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'SELECT sift.col_merge(''ws_i'', true) > 0',
	format('INSERT INTO peaks SELECT %s, ''merge'', scope, peak FROM sift._test_mem_highwater(true) WHERE scope LIKE ''col_writer%%''', n),
	'DROP TABLE ws']) WITH ORDINALITY AS t(s, k) ORDER BY n, k
\gexec

SELECT stage, scope, count(*) AS sizes FROM peaks GROUP BY stage, scope ORDER BY stage, scope;

-- while the units come: the same bytes at every size, to two file buffers
SELECT stage, max(peak) - min(peak) <= 2 * 8192 AS same_bytes FROM peaks WHERE scope = 'col_writer_spool' GROUP BY stage ORDER BY stage;

-- while the blocks are built: under the bound of its parts, a reader block of 8 kB for each of the 64 runs a merge reads (512 kB), and for each of the four leaves its buffers, runs and blocks (about 100 kB)
SELECT stage, max(peak) < 1572864 AS under_bound FROM peaks WHERE scope = 'col_writer_order' GROUP BY stage ORDER BY stage;

DROP TABLE peaks;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
