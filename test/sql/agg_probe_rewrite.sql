-- A join to a large table on a btree column, and an EXISTS, NOT EXISTS or IN over one, has the same rows as the lateral child that is a probe of the btree for each key the scan meets;
-- the planner makes that form too and keeps it where it costs less.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET jit = off;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

-- NULL keys in a tenth of the events, three thousand keys, ten events to each
CREATE TABLE ev (id int, fk int, v int, d date) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 10 = 0 THEN NULL ELSE i % 3000 END, i % 7, date '2026-01-01' + i % 300 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, fk, v, d);
-- ten rows to a key, five statuses, no row for a key that is a multiple of seven
CREATE TABLE lk (id int NOT NULL, parent int NOT NULL, status int, label text, weight int) WITH (autovacuum_enabled = false);
INSERT INTO lk SELECT i, i % 3000, (i / 3000) % 5, 'l' || (i % 3000 % 3), i % 4 FROM generate_series(0, 29999) i WHERE i % 3000 % 7 <> 0;
INSERT INTO lk SELECT i, i % 3000, (i / 3000) % 5, 'l' || (i % 3000 % 3), i % 4 FROM generate_series(30000, 36999) i WHERE i % 3000 % 7 <> 0;
CREATE INDEX lk_cover ON lk (parent, status) INCLUDE (label, weight);
-- a table of seven rows, below the size a probe is made for
CREATE TABLE dm (v int);
INSERT INTO dm SELECT generate_series(0, 6);
VACUUM (FREEZE, ANALYZE) ev, lk, dm;

CREATE FUNCTION probe_q(keys text, kind text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE kind
		WHEN 'inner' THEN format('SELECT lk.label, count(*), sum(ev.v) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s AND ev.d IS NOT NULL GROUP BY 1 ORDER BY 1', keys)
		WHEN 'inner_covered' THEN format('SELECT lk.label, count(*), sum(lk.weight) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'left' THEN format('SELECT lk.label, count(*), sum(ev.v), count(lk.id) FROM ev LEFT JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'left_null_keys' THEN format('SELECT lk.label, count(*), sum(ev.v) FROM ev LEFT JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.id BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'semi' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.fk BETWEEN %s AND EXISTS (SELECT 1 FROM lk WHERE lk.parent = ev.fk AND lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'anti' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.fk BETWEEN %s AND NOT EXISTS (SELECT 1 FROM lk WHERE lk.parent = ev.fk AND lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'inner_heap' THEN format('SELECT lk.label, count(*), sum(lk.id) FROM ev JOIN lk ON lk.parent = ev.fk WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'system_column' THEN format('SELECT lk.label, count(*) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s AND lk.ctid IS NOT NULL GROUP BY 1 ORDER BY 1', keys)
		WHEN 'comma_lateral' THEN format('SELECT x.w, count(*) FROM ev, lk, LATERAL (SELECT ev.v + 1 AS w) x WHERE lk.parent = ev.fk AND lk.status = 2 AND ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'comma' THEN format('SELECT lk.label, count(*), sum(ev.v) FROM ev, lk WHERE lk.parent = ev.fk AND lk.status = 2 AND ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'comma_three' THEN format('SELECT lk.label, count(*), sum(dm.v) FROM dm, ev, lk WHERE ev.v = dm.v AND lk.parent = ev.fk AND lk.status = 2 AND ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'not_in' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.fk BETWEEN %s AND ev.fk NOT IN (SELECT lk.parent FROM lk WHERE lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'not_in_nullable' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.v < 3 AND ev.id BETWEEN %s AND ev.fk NOT IN (SELECT lk.parent FROM lk WHERE lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'sublink' THEN format('SELECT ev.v, count(*) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s AND (SELECT count(*) FROM dm WHERE dm.v = lk.weight) > 0 GROUP BY 1 ORDER BY 1', keys)
		WHEN 'sublink_in_list' THEN format('SELECT ev.v, count(*), max((SELECT dm.v FROM dm WHERE dm.v = lk.weight + 1)) FROM ev LEFT JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'whole_row' THEN format('SELECT ev.v, count(lk), sum(length(lk::text)), count(*) FROM ev LEFT JOIN lk ON lk.parent = ev.fk AND lk.status = 2 WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'join_alias' THEN format('SELECT j.label, count(*), sum(j.weight) FROM (ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2) AS j (eid, fk, v, d, lid, parent, status, label, weight) WHERE j.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'anti_two_tables' THEN format('SELECT dm.v, count(*) FROM ev, dm WHERE ev.v = dm.v AND ev.fk BETWEEN %s AND NOT EXISTS (SELECT 1 FROM lk WHERE lk.parent = ev.fk AND lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
		WHEN 'lateral_entry' THEN format('SELECT x.w, count(*) FROM ev JOIN lk ON lk.parent = ev.fk AND lk.status = 2, LATERAL (SELECT lk.weight + ev.v AS w) x WHERE ev.fk BETWEEN %s GROUP BY 1 ORDER BY 1', keys)
		WHEN 'in' THEN format('SELECT ev.v, count(*) FROM ev WHERE ev.fk BETWEEN %s AND ev.fk IN (SELECT lk.parent FROM lk WHERE lk.status = 4 AND lk.label = ''l1'') GROUP BY 1 ORDER BY 1', keys)
	END
$$;

-- whether the statement runs a probe of the table's btree for each key
CREATE FUNCTION probes(sql text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text; r boolean := false; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		r := r OR l ~ 'Lookup Joins: .*sift_probe';
	END LOOP;
	RETURN r;
END $$;

SELECT '100 AND 109' AS few \gset

-- a few keys: the probes, and no bitmap scan in the child of one that fetches rows from the heap (the scan walks an index scan in its btree)
CREATE FUNCTION bitmap_child(sql text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text; r boolean := false; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		r := r OR l ~ 'Bitmap';
	END LOOP;
	RETURN r;
END $$;
SELECT bitmap_child(probe_q(:'few', 'inner_heap')) AS bitmap_child;
-- the same rows read as a statement of their own are a bitmap scan
SELECT bitmap_child('SELECT lk.id FROM lk WHERE lk.parent = 100') AS bitmap_as_written;
SELECT kind, probes(probe_q(:'few', kind)) AS few_keys
FROM (VALUES ('inner'), ('inner_covered'), ('inner_heap'), ('left'), ('semi'), ('anti'), ('in'), ('comma'), ('comma_three'), ('not_in'), ('not_in_nullable'), ('system_column'), ('comma_lateral'), ('sublink'), ('sublink_in_list'), ('whole_row'), ('join_alias'), ('anti_two_tables'), ('lateral_entry')) q(kind) ORDER BY 1;
SET sift.agg_probe_rewrite = off;
SELECT kind, probes(probe_q(:'few', kind)) AS off FROM (VALUES ('inner'), ('semi')) q(kind) ORDER BY 1;
RESET sift.agg_probe_rewrite;

-- the answers: the same without the rewrite and from the ordinary plan, in a serial and a parallel scan; keys that no row of the table has; the events without a key; keys twice
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
	FROM (VALUES ('few', '100 AND 109'), ('one', '7 AND 7'), ('absent', '5000 AND 5004'), ('multiples_of_seven', '0 AND 20'), ('every', '0 AND 2999')) k(name, range),
		 (VALUES ('inner'), ('inner_covered'), ('inner_heap'), ('left'), ('semi'), ('anti'), ('in'), ('comma'), ('comma_three'), ('not_in'), ('not_in_nullable'), ('system_column'), ('comma_lateral'), ('sublink'), ('sublink_in_list'), ('whole_row'), ('join_alias'), ('anti_two_tables'), ('lateral_entry')) q(kind), (VALUES (0), (3)) w(workers)
	WHERE NOT (q.kind = 'left_null_keys')
	ORDER BY 1, 2, 3
$$;
SELECT * FROM compare_all();
-- the events without a key (a tenth of them: a NULL meets no row of the table, and the left join and NOT EXISTS keep them)
SELECT kind, workers, answers('1 AND 400', kind, 'probe', workers) = answers('1 AND 400', kind, 'normal', workers) AS same_answers
FROM (VALUES ('left_null_keys')) q(kind), (VALUES (0), (3)) w(workers) ORDER BY 1, 2;

-- rows changed since the last VACUUM, deleted, updated and inserted in the transaction, and rolled back
UPDATE lk SET label = 'l1' WHERE parent BETWEEN 100 AND 104 AND status = 2;
DELETE FROM lk WHERE parent = 106 AND status IN (2, 4);
BEGIN;
INSERT INTO lk SELECT 90000 + i, 105, 4, 'l1', 1 FROM generate_series(1, 3) i;
INSERT INTO lk SELECT 91000 + i, 107, 2, 'l2', 1 FROM generate_series(1, 3) i;
SELECT kind, workers, answers('100 AND 109', kind, 'probe', workers) = answers('100 AND 109', kind, 'normal', workers) AS same_answers
FROM (VALUES ('inner'), ('left'), ('semi'), ('anti'), ('in'), ('comma'), ('not_in'), ('whole_row')) q(kind), (VALUES (0), (3)) w(workers) ORDER BY 1, 2;
ROLLBACK;
SELECT kind, workers, answers('100 AND 109', kind, 'probe', workers) = answers('100 AND 109', kind, 'normal', workers) AS same_answers
FROM (VALUES ('inner'), ('left'), ('semi'), ('anti'), ('in'), ('comma'), ('not_in'), ('whole_row')) q(kind), (VALUES (0), (3)) w(workers) ORDER BY 1, 2;

-- permissions: the sub-select reads the table as the statement does (a role that may read the events and not the table of the join is refused, by the sub-select too)
CREATE FUNCTION denied(sql text) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN EXECUTE sql; RETURN false; EXCEPTION WHEN insufficient_privilege THEN RETURN true; END $$;
CREATE ROLE probe_reader;
GRANT SELECT ON ev TO probe_reader;
SET ROLE probe_reader;
SELECT kind, denied(probe_q(:'few', kind)) AS denied FROM (VALUES ('inner'), ('left'), ('semi'), ('anti')) q(kind) ORDER BY 1;
RESET ROLE;
REVOKE ALL ON ev FROM probe_reader;
DROP ROLE probe_reader;

DROP FUNCTION bitmap_child(text), compare_all(), answers(text, text, text, int), denied(text), probes(text), probe_q(text, text);
DROP TABLE ev, lk, dm;
DROP EXTENSION pg_sift;
