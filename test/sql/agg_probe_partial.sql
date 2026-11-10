-- A join to a large table whose only btree on the join column is a partial one (the soft-delete index `(parent) WHERE deleted_at IS NULL`) is a probe of that btree for each
-- key the scan meets when the join's own conditions imply the index predicate; where they do not, the table is read whole.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET jit = off;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE ev (id int, fk int, v int, d date) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 10 = 0 THEN NULL ELSE i % 3000 END, i % 7, date '2026-01-01' + i % 300 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, fk, v, d);
-- ten links to a key, a fifth of them deleted, none for a key that is a multiple of seven
CREATE TABLE lk (id int NOT NULL, parent int NOT NULL, status int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO lk SELECT i, i % 3000, i % 5, CASE WHEN i % 5 = 0 THEN timestamptz '2026-02-01' END FROM generate_series(0, 36999) i WHERE i % 3000 % 7 <> 0;
CREATE INDEX lk_live ON lk (parent) WHERE deleted_at IS NULL;
VACUUM (FREEZE, ANALYZE) ev, lk;

CREATE FUNCTION probe_q(keys text, kind text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE kind
		WHEN 'inner' THEN format('SELECT lk.status, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.deleted_at IS NULL WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'left' THEN format('SELECT lk.status, count(*), sum(ev.v), count(lk.id) FROM ev LEFT JOIN lk ON lk.parent = ev.fk AND lk.deleted_at IS NULL WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'comma' THEN format('SELECT lk.status, count(*), sum(ev.v) FROM ev, lk WHERE lk.parent = ev.fk AND lk.deleted_at IS NULL AND ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'status' THEN format('SELECT lk.status, count(*) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.deleted_at IS NULL AND lk.status = 2 WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'no_predicate' THEN format('SELECT lk.status, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'deleted' THEN format('SELECT lk.status, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.deleted_at IS NOT NULL WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'other_column' THEN format('SELECT lk.status, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'range' THEN format('SELECT lk.status, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.deleted_at < timestamptz ''2026-03-01'' WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
	END
$$;

CREATE FUNCTION probes(sql text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text; r boolean := false; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		r := r OR l ~ 'Lookup Joins: .*sift_probe';
	END LOOP;
	RETURN r;
END $$;

-- the join conditions that imply the predicate probe; the others do not (the rows with a deleted link, the other column, a range that implies nothing the index holds)
SELECT kind, probes(probe_q('111 AND 119', kind)) AS few_keys
FROM (VALUES ('inner'), ('left'), ('comma'), ('status'), ('no_predicate'), ('deleted'), ('other_column'), ('range')) q(kind) ORDER BY 1;

-- the answers: the same without the rewrite and from the ordinary plan, in a serial and a parallel scan; keys that no row has; the events without a key
CREATE FUNCTION answers(range text, kind text, mode text, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.agg_probe_rewrite', (mode = 'probe')::text, true);
	PERFORM set_config('sift.enable_agg', (mode <> 'normal')::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || probe_q(range, kind) || ') t' INTO r;
	RETURN coalesce(r, '');
END $$;
CREATE FUNCTION compare_all() RETURNS TABLE (name text, kind text, workers int, same_answers boolean, not_empty boolean) LANGUAGE sql AS $$
	SELECT k.name, q.kind, w.workers,
		   answers(k.range, q.kind, 'probe', w.workers) = answers(k.range, q.kind, 'whole', w.workers) AND answers(k.range, q.kind, 'probe', w.workers) = answers(k.range, q.kind, 'normal', w.workers),
		   answers(k.range, q.kind, 'probe', w.workers) <> ''
	FROM (VALUES ('few', '111 AND 119'), ('one', '8 AND 8'), ('absent', '5000 AND 5004'), ('every', '0 AND 2999')) k(name, range),
		 (VALUES ('inner'), ('left'), ('comma'), ('status'), ('no_predicate'), ('deleted'), ('other_column'), ('range')) q(kind),
		 (VALUES (0), (3)) w(workers)
	ORDER BY 1, 2, 3
$$;
SELECT * FROM compare_all();

-- links deleted, restored and added since the last VACUUM, in the transaction, and rolled back
UPDATE lk SET deleted_at = timestamptz '2026-02-01' WHERE parent BETWEEN 111 AND 115 AND status = 1;
UPDATE lk SET deleted_at = NULL WHERE parent BETWEEN 115 AND 118 AND status = 0;
BEGIN;
INSERT INTO lk SELECT 90000 + i, 116, 1, NULL FROM generate_series(1, 3) i;
DELETE FROM lk WHERE parent = 117;
SELECT kind, workers, answers('111 AND 119', kind, 'probe', workers) = answers('111 AND 119', kind, 'normal', workers) AS same_answers
FROM (VALUES ('inner'), ('left'), ('comma'), ('status'), ('deleted')) q(kind), (VALUES (0), (3)) w(workers) ORDER BY 1, 2;
ROLLBACK;
SELECT kind, workers, answers('111 AND 119', kind, 'probe', workers) = answers('111 AND 119', kind, 'normal', workers) AS same_answers
FROM (VALUES ('inner'), ('left'), ('comma'), ('status'), ('deleted')) q(kind), (VALUES (0), (3)) w(workers) ORDER BY 1, 2;

DROP FUNCTION compare_all(), answers(text, text, text, int), probes(text), probe_q(text, text);
DROP TABLE ev, lk;
DROP EXTENSION pg_sift;
