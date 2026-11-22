-- The four surfaces that tell the state of a database: sift.indexes (one row per relation, to read), sift.status (one row of numbers per relation, to scrape), sift.health (what to do, with the
-- statement) and sift.activity (what runs now). Each lists a relation of Format 1, a quarantined one, an invalid one and a locked one as a row, and none of them fails on those.
-- A relation of Format Major 1 is fully supported: it is reported calmly (state ready, severity info) and never as an error.

-- sift.health: one row per thing to do, error first, then warn, then info; no row when everything is as it should be. action_sql is a statement to run (or a setting to change).
-- Format 1 is never an error: a sift_col index or sift_table of Format 1 is listed once as info, with the optional upgrade.
CREATE OR REPLACE FUNCTION sift.health(rel regclass DEFAULT NULL)
RETURNS TABLE (relation regclass, severity text, check_name text, detail text, action_sql text)
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift AS $fn$
	WITH cfg AS (
		SELECT coalesce((SELECT setting FROM pg_settings WHERE name = 'sift.async_maintenance'), 'on') = 'off' AS async_off,
			   coalesce((SELECT setting::int8 FROM pg_settings WHERE name = 'sift.shared_cache_kb'), 65536) AS shared_kb
	), st AS (
		SELECT s.*, i.indisvalid AND i.indisready AS is_valid, h.relpages, h.relallvisible, w.error_code, w.last_error, w.next_retry,
			   s.live_rows + s.dead_rows AS total_rows
		FROM sift.status(rel) s
		LEFT JOIN pg_index i ON i.indexrelid = s.relation
		LEFT JOIN pg_class h ON h.oid = s.table_name
		LEFT JOIN sift.workers w ON w.relation = s.relation
	), found AS (
		SELECT s.relation, 'info' AS severity, 'format_upgrade_available' AS check_name,
			   'Format 1.x (fully supported; optional REINDEX INDEX CONCURRENTLY upgrades to Format 2.0)' AS detail,
			   format('REINDEX INDEX CONCURRENTLY %s;', s.relation) AS action_sql
		FROM st s WHERE s.am = 'sift_col' AND s.format_major < 2 AND s.size_bytes > 0 AND s.is_valid
		UNION ALL
		SELECT s.relation, 'info', 'format_upgrade_available', 'Format 1.x (fully supported; optional upgrade to Format 2.0)',
			   format('SELECT * FROM sift.upgrade_table(%L);', s.relation::text)
		FROM st s WHERE s.am = 'sift_table' AND s.format_major < 2 AND s.size_bytes > 0
		UNION ALL
		SELECT s.relation, 'error', 'index_quarantined', 'Quarantined: ' || coalesce(s.quarantine_reason, 'out of service'),
			   format('REINDEX INDEX CONCURRENTLY %s;', s.relation)
		FROM st s WHERE s.quarantined
		UNION ALL
		SELECT s.relation, 'error', 'index_invalid', 'Interrupted concurrent index build left an invalid index',
			   format('DROP INDEX CONCURRENTLY %s;', s.relation)
		FROM st s WHERE NOT s.is_valid AND NOT EXISTS (SELECT 1 FROM pg_stat_progress_create_index p WHERE p.index_relid = s.relation)
		UNION ALL
		SELECT s.relation, CASE WHEN s.vblocks_left < 85899345 THEN 'error' ELSE 'warn' END, 'vblocks_low',
			   format('Only %s%% of virtual TID blocks remain (%s left)', round(100.0 * s.vblocks_left / 4294967294, 1), s.vblocks_left),
			   format('VACUUM %s;', s.relation)
		FROM st s WHERE s.am = 'sift_table' AND s.vblocks_left < 429496729
		UNION ALL
		SELECT s.relation, CASE WHEN s.maint_failures >= 5 THEN 'error' ELSE 'warn' END, 'worker_failing',
			   format('%s consecutive failures (last: %s, next retry %s)', s.maint_failures, coalesce(nullif(concat_ws(' ', s.error_code, s.last_error), ''), 'no error recorded'),
					  coalesce(s.next_retry::text, 'not scheduled')),
			   format('SELECT * FROM sift.maintain(%L);', s.relation::text)
		FROM st s WHERE s.maint_failures > 0
		UNION ALL
		SELECT s.relation, 'warn', 'tail_shrink_blocked',
			   format('%s (%s%%) free space blocked by %s', pg_size_pretty(s.free_bytes), round(100.0 * s.free_bytes / s.size_bytes, 1), s.shrink_blocked_by),
			   format('SELECT * FROM sift.maintain(%L);', s.relation::text)
		FROM st s WHERE s.free_bytes >= 4194304 AND s.free_bytes * 100 >= s.size_bytes * 15 AND s.shrink_blocked_by IS NOT NULL
		UNION ALL
		SELECT s.relation, 'warn', 'high_dead_rows', format('%s%% dead rows (%s rows)', s.dead_pct, s.dead_rows),
			   CASE WHEN c.async_off THEN 'ALTER SYSTEM SET sift.async_maintenance = on; SELECT pg_reload_conf();'
					ELSE format('SELECT sift.merge(%L);', s.relation::text) END
		FROM st s CROSS JOIN cfg c
		WHERE s.dead_pct >= 25 AND (c.async_off OR (s.total_rows >= 10000 AND coalesce(s.last_maint_at, '-infinity') < now() - interval '10 minutes'))
		UNION ALL
		SELECT s.relation, 'warn', 'l0_backlog', format('Write backlog: %s L0 parts, %s slabs awaiting compaction', coalesce(s.l0_parts, 0), coalesce(s.l0_slabs, 0)),
			   format('SELECT sift.promote(%L);', s.relation::text)
		FROM st s WHERE s.l0_parts >= 12 OR s.l0_slabs >= 32
		UNION ALL
		SELECT s.relation, 'warn', 'stale_scope', format('Scope "%s" has %s/%s dirty keys awaiting refresh', sc.scope_name, sc.dirty_keys, sc.nkeys),
			   format('SELECT * FROM sift.col_refresh(%L);', s.relation::text)
		FROM st s CROSS JOIN LATERAL sift.scope_stats(s.relation) sc
		WHERE s.am = 'sift_col' AND s.scopes > 0 AND sc.nkeys > 0 AND sc.dirty_keys * 100 > sc.nkeys * 10
		UNION ALL
		SELECT s.relation, 'info', 'heap_visibility_low',
			   format('Only %s%% of heap pages all-visible; scans perform extra heap vetting', round(100.0 * s.relallvisible / s.relpages)),
			   format('VACUUM %s;', s.table_name)
		FROM st s WHERE s.am = 'sift_col' AND s.relpages >= 64 AND s.relallvisible::numeric / s.relpages < 0.5
		UNION ALL
		SELECT NULL::regclass, 'info', 'shared_cache_pressure',
			   format('Shared cache (%s) at %s%% utilization (%s refusals)', pg_size_pretty(sum(p.limit_bytes)),
					  round(100.0 * sum(p.used_bytes) / sum(p.limit_bytes)), sum(p.refusals)),
			   format('ALTER SYSTEM SET sift.shared_cache_kb = %L; SELECT pg_reload_conf();', (SELECT shared_kb * 2 FROM cfg) || 'kB')
		FROM sift.shared_memory_pools() p
		WHERE rel IS NULL HAVING sum(p.limit_bytes) > 0 AND (sum(p.refusals) > 0 OR bool_or(NOT p.shared) OR 100.0 * sum(p.used_bytes) / sum(p.limit_bytes) >= 90)
		UNION ALL
		SELECT NULL::regclass, 'info', 'extension_not_preloaded', 'pg_sift is not preloaded; the first statement of each session is planned twice', sift_internal.preload_hint()
		WHERE rel IS NULL AND sift_internal.preload_hint() IS NOT NULL
	)
	SELECT f.relation, f.severity, f.check_name, f.detail, f.action_sql
	FROM found f
	ORDER BY CASE f.severity WHEN 'error' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END, f.relation::text NULLS LAST, f.check_name
$fn$;
COMMENT ON FUNCTION sift.health(regclass) IS 'What to do about the sift indexes and tables of the database (or about one), a row each, errors first: severity (error, warn, info), a stable check_name, the numbers behind it and action_sql, a statement to run. No row when all is well. A relation of Format 1 is listed as info (format_upgrade_available), never as an error.';

-- sift.activity: what the background maintenance and the foreground jobs do now. kind: worker (a job that runs), queued (a job that waits) or summary (one row: the workers, the pace the
-- maintenance keeps so as not to slow the statements of the database, and the shared memory). The columns of the summary are repeated on the other rows.
CREATE OR REPLACE FUNCTION sift.activity()
RETURNS TABLE (kind text, pid int4, relation regclass, part_id int8, job text, phase text, priority int4, done int8, total int8, progress_pct numeric(5,2),
			   bytes_written int8, elapsed_ms int8, throughput_mb_s numeric(8,2), eta_s numeric(10,1), started_at timestamptz, wait_event text,
			   workers_active int4, workers_max int4, pacer_mode text, pacer_delay_ms int4, oltp_active_backends int4,
			   shm_used_bytes int8, shm_limit_bytes int8, shm_util_pct numeric(5,2), shm_refusals int8)
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift AS $fn$
	WITH pc AS (
		SELECT * FROM sift_internal.maint_pacer()
	), shm AS (
		SELECT coalesce(sum(p.used_bytes), 0)::int8 AS used, coalesce((SELECT setting::int8 * 1024 FROM pg_settings WHERE name = 'sift.shared_cache_kb'), 0)::int8 AS lim,
			   coalesce(sum(p.refusals), 0)::int8 AS refusals
		FROM sift.shared_memory_pools() p
	), q AS (
		SELECT j.*, extract(epoch FROM now() - j.started_at) AS elapsed_s
		FROM sift_internal.maint_queue() j
		LEFT JOIN pg_index i ON i.indexrelid = j.relation
		WHERE sift._may_inspect(coalesce(i.indrelid, j.relation)::regclass)
	)
	SELECT CASE WHEN q.queued THEN 'queued' ELSE 'worker' END, q.pid, q.relation::regclass, q.part_id, q.job, q.phase, q.priority, q.done, q.total,
		   round(100.0 * q.done / NULLIF(q.total, 0), 2),
		   q.bytes_written, CASE WHEN NOT q.queued THEN (q.elapsed_s * 1000)::int8 END,
		   CASE WHEN NOT q.queued THEN round(q.bytes_written / 1048576.0 / NULLIF(q.elapsed_s, 0), 2) END,
		   CASE WHEN NOT q.queued AND q.done > 0 THEN round((q.elapsed_s * (q.total - q.done) / q.done)::numeric, 1) END,
		   q.started_at, (SELECT a.wait_event FROM pg_stat_activity a WHERE a.pid = q.pid),
		   pc.workers_active, pc.workers_max, pc.pacer_mode, pc.pacer_delay_ms, pc.oltp_active_backends,
		   shm.used, shm.lim, round(100.0 * shm.used / NULLIF(shm.lim, 0), 2), shm.refusals
	FROM q CROSS JOIN pc CROSS JOIN shm
	UNION ALL
	SELECT 'summary', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		   pc.workers_active, pc.workers_max, pc.pacer_mode, pc.pacer_delay_ms, pc.oltp_active_backends,
		   shm.used, shm.lim, round(100.0 * shm.used / NULLIF(shm.lim, 0), 2), shm.refusals
	FROM pc CROSS JOIN shm
$fn$;
COMMENT ON FUNCTION sift.activity() IS 'The maintenance jobs that run and wait now (kind worker and queued: relation, job, phase, priority, progress, bytes written, elapsed time, throughput, the time left, the wait event) and one summary row (the workers, the pace of the maintenance, the shared memory). The summary columns are on every row. Times are in ms and s, sizes in bytes.';

-- sift.indexes: the columns of 5.1 first (kind, table and bytes keep their names), then the 6.0 columns. size is for the eye, size_bytes for a program; a ratio is NULL when its denominator is 0.
CREATE OR REPLACE VIEW sift.indexes AS
	SELECT l.relation, l.kind, l."table", l.bytes, l.segments, l.format, l.state,
		   l."table" AS table_name, l.kind AS am, coalesce(h.health, 'ok') AS health,
		   s.live_rows + s.dead_rows AS rows, s.dead_pct, pg_size_pretty(l.bytes) AS size, l.bytes AS size_bytes,
		   s.b_per_row, s.compression_ratio, s.util_pct,
		   CASE WHEN s.format_major >= 2 THEN format('L0:%s L1:%s L2:%s (%s partns)', s.l0_parts, s.l1_parts, s.l2_parts, s.partitions)
				WHEN l.kind = 'sift' THEN format('%s segs (%s sealed)', s.l1_parts, s.l0_parts)
				WHEN s.format_major IS NOT NULL THEN format('%s segs, %s units (F1)', l.segments, coalesce(s.granules, 0)) END AS structure,
		   coalesce(s.scopes, 0)::int2 AS scopes, coalesce(s.quarantined, l.state = 'quarantined') AS quarantined, s.format_note
	FROM (SELECT c.oid::regclass AS relation, a.amname::text AS kind, i.indrelid::regclass AS "table", pg_relation_size(c.oid) AS bytes, w.segments,
				 f.major || '.' || f.minor AS format,
				 CASE WHEN NOT i.indisvalid OR NOT i.indisready THEN 'invalid' WHEN qs.quarantined THEN 'quarantined' WHEN w.segments IS NULL THEN 'locked'
					  WHEN w.failures > 0 THEN 'failing' WHEN w.running THEN 'maintaining' ELSE 'ready' END AS state
		  FROM pg_class c
		  JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
		  JOIN pg_index i ON i.indexrelid = c.oid
		  LEFT JOIN LATERAL sift._quarantine_state(CASE WHEN i.indisvalid AND i.indisready THEN c.oid END) qs ON true
		  LEFT JOIN LATERAL sift.worker_state(CASE WHEN i.indisvalid AND i.indisready AND NOT qs.quarantined THEN c.oid END) w ON true
		  LEFT JOIN LATERAL sift.index_format(CASE WHEN i.indisvalid AND i.indisready AND NOT qs.quarantined THEN c.oid END) f ON true
		  WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid)
		  UNION ALL
		  SELECT c.oid::regclass, a.amname::text, c.oid::regclass, pg_relation_size(c.oid), w.segments, f.major || '.' || f.minor,
				 CASE WHEN w.segments IS NULL THEN 'locked' WHEN w.failures > 0 THEN 'failing' WHEN w.running THEN 'maintaining' ELSE 'ready' END
		  FROM pg_class c
		  JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
		  CROSS JOIN LATERAL sift.worker_state(c.oid) w
		  CROSS JOIN LATERAL sift.index_format(c.oid) f
		  WHERE c.relkind IN ('r', 'm') AND sift._may_inspect(c.oid)) l
	LEFT JOIN sift.status() s ON s.relation = l.relation
	LEFT JOIN (SELECT k.relation, CASE WHEN bool_or(k.severity = 'error') THEN 'error' WHEN bool_or(k.severity = 'warn') THEN 'warn' ELSE 'info' END AS health
			   FROM sift.health() k WHERE k.relation IS NOT NULL GROUP BY k.relation) h ON h.relation = l.relation;
COMMENT ON VIEW sift.indexes IS 'One row per sift object of the database the caller can read, whatever its access method and format: relation, table, access method, format, state (invalid, locked, quarantined, failing, maintaining or ready), the worst severity of sift.health, rows, dead rows, size, bytes per row, compression ratio, space utilization, the parts by level, scopes and format_note. kind, table and bytes are the columns of 5.1 (am, table_name and size_bytes).';
GRANT SELECT ON sift.indexes TO PUBLIC;

-- sift.locks: the page locks of a sift index or sift_table named. A Format 2.0 relation locks virtual blocks at the top of the block range (a partition stripe, the relation, a bulk load, a worker, a split);
-- pg_locks.page is a signed int4, so a block above 2^31 is read back unsigned. The decoding of Format 1 and of the advisory locks is as in 5.1.
CREATE OR REPLACE VIEW sift.locks AS
	SELECT l.relation::regclass AS relation, k.kind, k.detail, l.mode, l.granted, l.waitstart AS waiting_since, l.pid
	FROM pg_locks l
	JOIN pg_class c ON c.oid = l.relation
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col', 'sift_table')
	LEFT JOIN pg_index i ON i.indexrelid = c.oid
	CROSS JOIN LATERAL (SELECT l.page::int8 + CASE WHEN l.page < 0 THEN 4294967296 ELSE 0 END AS blk) u
	CROSS JOIN LATERAL (SELECT CASE WHEN u.blk BETWEEN 4294963200 AND 4294964223 THEN 'partition'
									WHEN u.blk = 4294965248 THEN 'maintenance' WHEN u.blk = 4294965249 THEN 'load'
									WHEN u.blk = 4294965250 THEN 'worker' WHEN u.blk = 4294965251 THEN 'split'
									ELSE CASE u.blk WHEN 0 THEN 'maintenance' WHEN 2 THEN 'active segment' WHEN 3 THEN 'worker' WHEN 4 THEN 'tombstones'
													WHEN 5 THEN 'load' ELSE 'page' END END AS kind,
							   CASE WHEN u.blk BETWEEN 4294963200 AND 4294964223 THEN 'stripe ' || (u.blk - 4294963200)
									WHEN u.blk = 4294965248 THEN 'format2 relation' WHEN u.blk = 4294965249 THEN 'bulk load'
									WHEN u.blk = 4294965250 THEN 'maint worker' WHEN u.blk = 4294965251 THEN 'partition split / scopedir'
									ELSE 'block ' || u.blk END AS detail) k
	WHERE l.locktype = 'page' AND l.database = (SELECT oid FROM pg_database WHERE datname = current_database()) AND sift._may_inspect(COALESCE(i.indrelid, c.oid))
	UNION ALL
	SELECT CASE WHEN l.classid = 20984 AND l.objsubid = 3 THEN l.objid::regclass END, k.kind, k.detail, l.mode, l.granted, l.waitstart, l.pid
	FROM pg_locks l
	CROSS JOIN LATERAL (SELECT CASE WHEN l.classid = 20983 AND l.objid >= 1397948416::oid AND l.objid < 1397948480::oid THEN 'rewrite slot'
									WHEN l.classid = 20984 AND l.objsubid = 3 THEN 'table vacuum'
									WHEN l.classid = 20984 AND l.objid = 1::oid THEN 'orphan sweep' END AS kind,
							   CASE WHEN l.classid = 20983 THEN 'slot ' || (l.objid::text::int8 - 1397948416) END AS detail) k
	WHERE l.locktype = 'advisory' AND l.database = (SELECT oid FROM pg_database WHERE datname = current_database()) AND k.kind IS NOT NULL
	  AND (l.classid <> 20984 OR l.objsubid <> 3 OR sift._may_inspect(l.objid::regclass));
COMMENT ON VIEW sift.locks IS 'The locks of pg_sift held or waited for now, named: the page locks of an index or sift_table (maintenance, active segment, worker, tombstones, load; for Format 2.0 also a partition stripe and a split) and its advisory locks (a rewrite slot, the vacuum of a table, the sweep of orphan triggers), with the mode, whether it is granted, since when a session waits and which one holds it.';
