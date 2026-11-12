-- A scope read by the transaction that changed the pairs or the events. The journal record of a pair change carries the transaction and the command: the changes of the reader's own transaction (an
-- earlier command, an open savepoint) are changed pairs of its next reads and are read through the pairs under its snapshot; those of a savepoint that rolled back, or of a transaction that rolled back, are
-- void; a cursor opened before a change reads the pairs and the events as they were at its command. Every read is md5 equal to the join with the scope off and to the plain plan, takes the scope units
-- (no fallback), and the changes move the answer (the join answer differs from that of the committed state), so a read that ignored them would fail here. Counts and hashes only, no timings.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
-- the narrowed read is taken whatever it costs: the price is the subject of col_scope_patch_price
SET sift.test_scope_patch = on;

-- 8 keys; dim_a_id d is in 1 + d % 3 of them (1 to 100), 101..120 have events and no pair
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT d, 1 + (d * 5 + j * 7) % 8 FROM generate_series(1, 100) d, generate_series(0, d % 3) j ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 120 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 8000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

-- the read shapes of col_scope_changed_pairs, ordered (a cursor reads them in order)
CREATE TABLE shapes (n int, label text, stmt text);
INSERT INTO shapes VALUES
	(1, 'every key, count and sum', 'SELECT m.tenant_id AS k, count(*), sum(amount) FROM events f JOIN tenant_scope m ON m.dim_a_id = f.dim_a_id GROUP BY 1 ORDER BY 1'),
	(2, 'one key, count and sum', 'SELECT count(*), sum(amount) FROM events f WHERE f.dim_a_id IN (SELECT dim_a_id FROM tenant_scope m WHERE m.tenant_id = 3)'),
	(3, 'a list of keys, a row once', 'SELECT count(*), sum(amount) FROM events f WHERE f.dim_a_id IN (SELECT dim_a_id FROM tenant_scope m WHERE m.tenant_id IN (3, 5, 7))'),
	(4, 'three keys, a row for each pair', 'SELECT count(*), sum(amount), min(id) FROM events f JOIN tenant_scope m ON m.dim_a_id = f.dim_a_id WHERE m.tenant_id IN (3, 5, 7)');

-- the md5 of the rows of a statement with the scope read and the sift plan each on or off; the settings are those of the transaction, put back at the end
CREATE FUNCTION own_hash(q text, sift boolean, scope boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	h text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN sift THEN 'on' ELSE 'off' END, true), set_config('sift.agg_scope', CASE WHEN scope THEN 'on' ELSE 'off' END, true);
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO h;
	PERFORM set_config('sift.enable_agg', 'on', true), set_config('sift.agg_scope', 'on', true);
	RETURN h;
END $$;

-- the route of a statement: "units" when the plan reads the scope units, else the fallback reason or "no scope read"
CREATE FUNCTION own_route(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	RETURN coalesce(jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}', CASE WHEN jsonb_path_exists(p, '$.**."Scope Units Read"') THEN 'units' ELSE 'no scope read' END);
END $$;

-- the answers of the committed state, to say that a change moved them
CREATE TABLE own_base AS SELECT n, own_hash(stmt, false, false) AS h FROM shapes;

-- the committed state of now is the state a change moves from
CREATE FUNCTION own_rebase() RETURNS void LANGUAGE sql AS $$
	DELETE FROM own_base;
	INSERT INTO own_base SELECT n, own_hash(stmt, false, false) FROM shapes
$$;

-- the shapes at the state of this transaction: equal to the join and to the plain plan, the route, and whether the answer moved from the committed state
CREATE FUNCTION own_probe(state text) RETURNS TABLE (state text, n int, verdict text, route text, moved boolean) LANGUAGE sql AS $$
	SELECT $1, s.n, CASE WHEN own_hash(s.stmt, true, true) = own_hash(s.stmt, true, false) AND own_hash(s.stmt, true, true) = own_hash(s.stmt, false, false) THEN 'same' ELSE 'DIFFERENT' END,
		own_route(s.stmt), own_hash(s.stmt, false, false) <> b.h
	FROM shapes s JOIN own_base b USING (n) ORDER BY s.n
$$;
SELECT * FROM own_probe('committed');

-- earlier commands of one transaction: each change is read by the next command, the transaction rolls back at the end and so do its records
BEGIN;
INSERT INTO tenant_scope SELECT d, t FROM generate_series(1, 12) d, unnest(ARRAY[3, 5, 7]) t UNION ALL SELECT d, 3 FROM generate_series(111, 114) d ON CONFLICT DO NOTHING;
SELECT * FROM own_probe('pairs added');
DELETE FROM tenant_scope WHERE tenant_id = 3 AND dim_a_id % 4 = 0;
SELECT * FROM own_probe('pairs removed');
WITH d AS (DELETE FROM tenant_scope WHERE dim_a_id % 7 = 0 RETURNING dim_a_id, tenant_id) INSERT INTO tenant_scope SELECT dim_a_id, 1 + tenant_id % 8 FROM d ON CONFLICT DO NOTHING;
SELECT * FROM own_probe('pairs moved');
UPDATE tenant_scope p SET tenant_id = 5 WHERE dim_a_id BETWEEN 20 AND 40 AND tenant_id = 3 AND NOT EXISTS (SELECT 1 FROM tenant_scope x WHERE x.dim_a_id = p.dim_a_id AND x.tenant_id = 5);
SELECT * FROM own_probe('a pair updated');
INSERT INTO events SELECT 8000 + i, 1 + i % 120, timestamptz '2026-02-05', 4, 'u' FROM generate_series(1, 100) i;
SELECT * FROM own_probe('events inserted (buffered)');
INSERT INTO tenant_scope VALUES (118, 5);
DELETE FROM tenant_scope WHERE dim_a_id = 118;
SELECT * FROM own_probe('a pair inserted and deleted by the transaction');
ROLLBACK;
SELECT * FROM own_probe('after the rollback');

-- savepoints: the records of a savepoint that rolled back are void, those of the others stay
BEGIN;
SAVEPOINT a;
INSERT INTO tenant_scope SELECT d, t FROM generate_series(1, 10) d, unnest(ARRAY[3, 5, 7]) t UNION ALL SELECT 115, 7 ON CONFLICT DO NOTHING;
DELETE FROM tenant_scope WHERE tenant_id = 5 AND dim_a_id % 5 = 0;
SELECT * FROM own_probe('inside the savepoint');
ROLLBACK TO a;
SELECT * FROM own_probe('after ROLLBACK TO');
INSERT INTO tenant_scope SELECT d, t FROM generate_series(13, 24) d, unnest(ARRAY[7, 3]) t ON CONFLICT DO NOTHING;
SAVEPOINT b;
DELETE FROM tenant_scope WHERE tenant_id = 7 AND dim_a_id % 3 = 0;
INSERT INTO events SELECT 9000 + i, 1 + i % 120, timestamptz '2026-02-06', 7, 'v' FROM generate_series(1, 60) i;
SELECT * FROM own_probe('a change after ROLLBACK TO, and a nested savepoint');
ROLLBACK TO b;
SELECT * FROM own_probe('the nested savepoint rolled back, the outer change stays');
SAVEPOINT c;
UPDATE tenant_scope p SET tenant_id = 3 WHERE tenant_id = 7 AND dim_a_id % 2 = 0 AND NOT EXISTS (SELECT 1 FROM tenant_scope x WHERE x.dim_a_id = p.dim_a_id AND x.tenant_id = 3);
RELEASE c;
SELECT * FROM own_probe('a released savepoint');
COMMIT;
SELECT * FROM own_probe('committed');
SELECT own_rebase();

-- the savepoint of a plpgsql block that fails: the exception rolls its changes back
DO $$
BEGIN
	BEGIN
		INSERT INTO tenant_scope SELECT d, t FROM generate_series(25, 40) d, unnest(ARRAY[3, 5, 7]) t ON CONFLICT DO NOTHING;
		DELETE FROM tenant_scope WHERE tenant_id = 3 AND dim_a_id % 6 = 1;
		RAISE EXCEPTION 'undo';
	EXCEPTION WHEN raise_exception THEN
		NULL;
	END;
END $$;
SELECT * FROM own_probe('after an exception block');

-- a cursor reads as of its command: the changes of later commands of its own transaction are not in it (the cursor with the scope read, the cursor of the plain plan and the join before the
-- change give one answer), a statement after them reads them
CREATE FUNCTION own_cursor(q text, dml text, OUT cursors_agree boolean, OUT as_before boolean, OUT moved_since boolean) LANGUAGE plpgsql AS $$
DECLARE
	c1 refcursor;
	c2 refcursor;
	r record;
	a text[] := '{}';
	b text[] := '{}';
	before text := own_hash(q, false, false);
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true), set_config('sift.agg_scope', 'on', true);
	OPEN c1 FOR EXECUTE q;
	PERFORM set_config('sift.enable_agg', 'off', true);
	OPEN c2 FOR EXECUTE q;
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE dml;
	LOOP
		FETCH c1 INTO r;
		EXIT WHEN NOT FOUND;
		a := a || r::text;
	END LOOP;
	LOOP
		FETCH c2 INTO r;
		EXIT WHEN NOT FOUND;
		b := b || r::text;
	END LOOP;
	cursors_agree := (SELECT array_agg(x ORDER BY x) FROM unnest(a) x) IS NOT DISTINCT FROM (SELECT array_agg(x ORDER BY x) FROM unnest(b) x);
	as_before := md5(coalesce((SELECT string_agg(x, '|' ORDER BY x) FROM unnest(a) x), '')) = before;
	moved_since := own_hash(q, false, false) <> before;
END $$;
BEGIN;
SELECT s.n, c.* FROM shapes s, LATERAL own_cursor(s.stmt, 'INSERT INTO tenant_scope SELECT d, t FROM generate_series(1, 30) d, unnest(ARRAY[3, 5, 7]) t UNION ALL SELECT d, 5 FROM generate_series(111, 114) d ON CONFLICT DO NOTHING') c WHERE s.n = 1;
SELECT s.n, c.* FROM shapes s, LATERAL own_cursor(s.stmt, 'DELETE FROM tenant_scope WHERE tenant_id IN (3, 5, 7) AND dim_a_id % 4 = 0') c WHERE s.n = 3;
SELECT s.n, c.* FROM shapes s, LATERAL own_cursor(s.stmt, 'INSERT INTO events SELECT 8000 + i, 1 + i % 120, timestamptz ''2026-02-05'', 4, ''u'' FROM generate_series(1, 100) i') c WHERE s.n = 4;
SELECT s.n, c.* FROM shapes s, LATERAL own_cursor(s.stmt, 'UPDATE tenant_scope p SET tenant_id = 3 WHERE tenant_id = 5 AND dim_a_id % 2 = 0 AND NOT EXISTS (SELECT 1 FROM tenant_scope x WHERE x.dim_a_id = p.dim_a_id AND x.tenant_id = 3)') c WHERE s.n = 2;
SELECT * FROM own_probe('after the cursors');
ROLLBACK;
SELECT * FROM own_probe('after the rollback of the cursors');
SELECT sift.col_verify('events_col', true);

DROP TABLE shapes, own_base, events, tenant_scope;
DROP FUNCTION own_hash(text, boolean, boolean), own_route(text), own_rebase(), own_probe(text), own_cursor(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
