-- The memory of an index build is bounded the way PostgreSQL bounds the memory of a sort or a GIN accumulator: it follows maintenance_work_mem (here the budget of a run is scaled down to 2 MB with
-- sift.test_col_segment_kb, so that tables of a few hundred thousand rows fill many runs, as a table of hundreds of millions fills them at the default) and not the rows of the table.
-- sift.test_col_build_peak_kb is the most the contexts of the backend held above what they held when the build began (the plain index, a scope of 1 and of 3 entries a row; CREATE INDEX, REINDEX
-- and CREATE INDEX CONCURRENTLY). For each kind the peak stays within the working set (a few superblocks of rows, their dictionaries and the encoded segment) above 1.25 times maintenance_work_mem, and the
-- peak of the larger table exceeds that of the smaller by at most a few MB once the rows fill the budget (the two largest sizes; the concurrent scope of 3 entries a row is also built with 64 MB): nothing is kept for each row, entry or run in a context that outlives the run that made it.
-- The answers of the scope index built this way are the join's (the entries are the multiset of (address, scope key) of the heap and the pairs), and the index is verified.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET client_min_messages = warning;
SET maintenance_work_mem = '4MB';
SET sift.test_col_segment_kb = 2048;

CREATE TABLE peaks (kind text, mwm_kb int, nrows int, kb int);

-- events with a member in 1..nrows/20; every member is in per_member keys of 40, so a row has per_member entries
CREATE FUNCTION lab(nrows int, per_member int) RETURNS void LANGUAGE plpgsql AS
$$ BEGIN
	DROP TABLE IF EXISTS events, tenant_scope CASCADE;
	CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
	INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, nrows / 20) m, generate_series(0, per_member - 1) j;
	CREATE TABLE events (id bigint PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL, d2 date, region text, status text, name text,
						 amt numeric(12,2), flag boolean, doc jsonb) WITH (autovacuum_enabled = false);
	INSERT INTO events
		SELECT i, 1 + ((i::bigint * 7919) % (nrows / 20))::int, timestamptz '2026-01-01' + (i % 100000) * interval '1 minute', i % 7, i % 1000, date '2025-01-01' + i % 700,
			   'r' || i % 40, (ARRAY['new', 'paid', 'void', 'late'])[1 + i % 4], 'name-' || i % 5000, (i % 100000) / 100.0, i % 3 = 0,
			   jsonb_build_object('a', 'loc' || i % 30, 'b', i % 90, 'c', 'k' || i % 500)
		FROM generate_series(1, nrows) i;
	ANALYZE events;
	ANALYZE tenant_scope;
END $$;

-- every build is a statement of its own after its tables are committed, as the build of an application is: the steps are rows that \gexec runs in order
CREATE TABLE plan (n serial, kind text, mwm_kb int, nrows int, per_member int, scoped boolean, how text);
INSERT INTO plan (kind, mwm_kb, nrows, per_member, scoped, how)
	SELECT k.kind, 4096, r.n, k.per_member, k.scoped, k.how
	FROM (VALUES ('plain', 0, false, 'create'), ('scope 1 entry', 1, true, 'create'), ('scope 3 entries', 3, true, 'create'), ('reindex scope 3', 3, true, 'reindex'),
				 ('concurrently plain', 0, false, 'concurrently'), ('concurrently scope 3', 3, true, 'concurrently')) k(kind, per_member, scoped, how),
		 (VALUES (50000), (100000), (200000)) r(n)
	UNION ALL
	SELECT 'concurrently scope 3', 65536, r.n, 3, true, 'concurrently' FROM (VALUES (100000), (200000), (400000), (800000)) r(n)
	UNION ALL
	SELECT 'concurrently scope 3', 4096, 400000, 3, true, 'concurrently'
	ORDER BY 4, 2, 1;

CREATE FUNCTION steps(p plan) RETURNS SETOF text LANGUAGE sql AS
$$ SELECT format('SET maintenance_work_mem = %s', p.mwm_kb)
   UNION ALL
   SELECT CASE WHEN p.mwm_kb = 4096 THEN 'SET sift.test_col_segment_kb = 2048' ELSE 'RESET sift.test_col_segment_kb' END
   UNION ALL
   SELECT format('SELECT lab(%s, %s)', p.nrows, p.per_member)
   UNION ALL
   SELECT format('CREATE INDEX %s events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num, d2, region, status, name, amt, flag, doc) WITH (paths = %L%s)',
				 CASE WHEN p.how = 'concurrently' THEN 'CONCURRENTLY' ELSE '' END, 'doc->a text, doc->b int, doc->c text',
				 CASE WHEN p.scoped THEN ', scope = ' || quote_literal('tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)') ELSE '' END)
   UNION ALL
   SELECT 'REINDEX INDEX events_col' WHERE p.how = 'reindex'
   UNION ALL
   SELECT format('INSERT INTO peaks VALUES (%L, %s, %s, current_setting(%L)::int)', p.kind, p.mwm_kb, p.nrows, 'sift.test_col_build_peak_kb') $$;

SELECT s FROM plan p, LATERAL steps(p) s ORDER BY p.n \gexec

\set WORK_KB 40960
\set SLOPE_KB 6144
WITH r AS (SELECT kind, mwm_kb, kb, dense_rank() OVER (PARTITION BY kind, mwm_kb ORDER BY nrows DESC) AS larger FROM peaks)
SELECT kind, mwm_kb, bool_and(kb <= mwm_kb * 5 / 4 + :WORK_KB) AS within_budget_and_working_set,
	   max(kb) FILTER (WHERE larger = 1) - max(kb) FILTER (WHERE larger = 2) <= :SLOPE_KB AS flat_in_rows
FROM r GROUP BY kind, mwm_kb ORDER BY mwm_kb, kind;

-- what the last build wrote (CREATE INDEX of the scope of 3 entries a row) answers as the join does, and the index is sound
CREATE FUNCTION entries_md5() RETURNS text LANGUAGE plpgsql AS
$$ BEGIN RETURN (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col')); END $$;
CREATE FUNCTION join_md5() RETURNS text LANGUAGE plpgsql AS
$$ BEGIN RETURN (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id); END $$;
SELECT entries_md5() = join_md5() AS entries_are_the_join, (SELECT rows FROM sift.col_index_info('events_col')) = (SELECT count(*) FROM events) AS rows_once;
SELECT sift.col_verify('events_col', true);
SELECT count(*) AS scope_problems FROM sift.col_scope_verify('events_col', 100);

DROP FUNCTION entries_md5();
DROP FUNCTION join_md5();
DROP FUNCTION steps(plan);
DROP FUNCTION lab(int, int);
DROP TABLE plan, peaks, events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
