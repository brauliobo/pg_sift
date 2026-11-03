-- What the default (track_changes = 'auto', a primary key) gives a read, in the real scenario: updates, key changes and hard deletes, then the same aggregate on a table whose index has the option
-- off and on the heap (no sift plan). The rows are the same three ways, and only the tracked index leaves the heap out: after each statement the plans are asserted to be SiftAgg plans, the tracked read
-- visits no heap page and the one of the index with the option off visits them.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int) WITH (autovacuum_enabled = false, fillfactor = 100);
CREATE TABLE ev_off (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO ev SELECT i, i % 10, i FROM generate_series(1, 20000) i;
INSERT INTO ev_off SELECT i, i % 10, i FROM generate_series(1, 20000) i;
CREATE INDEX ev_c ON ev USING sift_col (id, tenant_id, counter_a);
CREATE INDEX ev_off_c ON ev_off USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'off');
VACUUM ev, ev_off;

-- dynamic SQL: the plan is asserted to be a SiftAgg plan or not; a cached plan would keep the plan of the first call
CREATE FUNCTION answer(t text, want_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM %s GROUP BY tenant_id', t); plan text := ''; l text; r text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF (plan LIKE '%SiftAgg%') <> want_sift THEN
		RAISE EXCEPTION 'the plan is % a SiftAgg plan: %', CASE WHEN want_sift THEN 'not' ELSE 'still' END, plan;
	END IF;
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', tenant_id, n, s), '' '' ORDER BY tenant_id), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pages(t text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT tenant_id, count(*), sum(counter_a) FROM %s GROUP BY tenant_id', t) INTO j;
	RETURN coalesce(jsonb_path_query_first(j, 'strict $.**."Heap Pages"')::int, -1);
END $$;
-- one row per case: the tracked read, the read of the index with the option off and the heap give the same rows (the same statements ran on both tables), and the heap pages each read visited
CREATE FUNCTION chk(what text, OUT test text, OUT same_rows bool, OUT tracked_reads_heap bool, OUT untracked_reads_heap bool)
LANGUAGE plpgsql AS $$
DECLARE tracked text; untracked text; oracle text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	-- the pages first: PostgreSQL 19 marks a page all-visible in the visibility map while a read prunes it, so a second read of the same pages may find none to visit
	tracked_reads_heap := pages('ev') > 0;
	tracked := answer('ev', true);
	untracked_reads_heap := pages('ev_off') > 0;
	untracked := answer('ev_off', true);
	PERFORM set_config('sift.enable_agg', 'off', true);
	oracle := answer('ev', false);
	test := what;
	same_rows := tracked = untracked AND tracked = oracle AND tracked = answer('ev_off', false);
END $$;

SELECT * FROM chk('vacuumed, nothing changed');
UPDATE ev SET counter_a = counter_a + 5 WHERE id % 20 = 0;
UPDATE ev_off SET counter_a = counter_a + 5 WHERE id % 20 = 0;
SELECT * FROM chk('5% updated');
UPDATE ev SET id = id + 100000 WHERE id % 50 = 1;
UPDATE ev_off SET id = id + 100000 WHERE id % 50 = 1;
SELECT * FROM chk('keys changed');
DELETE FROM ev WHERE id % 40 = 3;
DELETE FROM ev_off WHERE id % 40 = 3;
SELECT * FROM chk('rows hard deleted');
SELECT sift.col_promote('ev_c') >= 0 AS promoted, sift.col_promote('ev_off_c') >= 0 AS promoted_off;
SELECT * FROM chk('promoted');
VACUUM ev, ev_off;
SELECT * FROM chk('vacuumed again');

DROP FUNCTION answer(text, bool), pages(text), chk(text);
DROP TABLE ev, ev_off;
DROP EXTENSION pg_sift;
