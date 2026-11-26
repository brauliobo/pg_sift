-- The scope summary describes the scope relation as of its build. A read of a scope key takes the pairs that changed since (the journal says which) and the rows of the write buffers along with
-- the summary (Scope Cube: delta), or reads the join when the summary is not exact for its snapshot. Every state is md5 equal to the heap (sift.enable_agg = off). The journal of the scope relation is
-- the real one (the pair relation has its journal index). A join counts a fact once for each pair of its member with the keys; EXISTS counts it once and is read by the plain node.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- q read by the node and by the heap: equal or not, and the Scope Read of the node (NULL: not a scope read)
CREATE FUNCTION scope_run(q text, OUT same boolean, OUT route text)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	route := jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}';
END $$;

-- the statement over the keys: the join (a fact for each pair) or EXISTS (a fact once)
CREATE FUNCTION keys_q(keys int8[], pairs boolean) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN pairs THEN format('SELECT count(*), sum(v), min(id), max(id) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = ANY (%L::int8[])', keys)
		ELSE format('SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE EXISTS (SELECT 1 FROM membership m WHERE m.dim_a_id = f.dim_a_id AND m.group_id = ANY (%L::int8[]))', keys) END
$$;

-- 12 groups; member m is in 1 + m % 6 of them (1 to 6), members 100..109 are in none, members 301..320 have facts and no pair; 20,000 padded facts, some without a member
CREATE TABLE membership (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO membership SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
CREATE INDEX membership_journal ON membership USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, at, v)
	WITH (scope = 'group_id: dim_a_id -> membership(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) membership;

-- one key, several keys (a fact once, IN and EXISTS) and the pairs of several keys (a fact once for each pair, as the join counts it)
CREATE TABLE probes (n int, keys int8[], pairs boolean);
INSERT INTO probes VALUES (1, '{3}', false), (2, '{12}', false), (3, '{7,9,11}', false), (4, '{1,2,3,4,5,6}', false), (5, '{2,9}', false), (6, '{99}', false),
	(7, '{3,5}', true), (8, '{1,2,3,4,5,6}', true), (9, '{5}', true), (10, '{12,6}', true), (11, '{2,7}', true), (12, '{4,10}', true);
CREATE FUNCTION probe() RETURNS TABLE (n int, verdict text, route text) LANGUAGE sql AS $$
	SELECT p.n, CASE WHEN r.same THEN 'same' ELSE 'DIFFERENT' END, coalesce(r.route, 'not a scope read')
	FROM probes p, LATERAL scope_run(keys_q(p.keys, p.pairs)) r
	ORDER BY p.n
$$;

-- built: nothing changed, the summary alone answers
SELECT * FROM probe();

-- pairs inserted (members in several keys get one more; a member with facts and no pair gets its first)
INSERT INTO membership VALUES (1, 12), (2, 12), (7, 12), (301, 12), (302, 3), (303, 5), (303, 3) ON CONFLICT DO NOTHING;
SELECT * FROM probe();

-- pairs deleted
DELETE FROM membership WHERE group_id = 3 AND dim_a_id % 5 = 0;
SELECT * FROM probe();

-- members moved between keys (the pair of a key deleted, the pair of another inserted)
WITH d AS (DELETE FROM membership WHERE dim_a_id % 7 = 0 RETURNING dim_a_id, group_id)
INSERT INTO membership SELECT dim_a_id, 1 + group_id % 12 FROM d ON CONFLICT DO NOTHING;
SELECT * FROM probe();

-- facts inserted (write buffer rows are not in the summary), for members of any number of keys, of none, and of no member
INSERT INTO facts SELECT 20000 + i, CASE WHEN i % 50 = 0 THEN NULL ELSE 1 + (i * 37) % 320 END, timestamptz '2026-02-01', 1, 'y' FROM generate_series(1, 400) i;
SELECT * FROM probe();

-- facts updated (a new version in the buffer, the old one dead in the summary) and deleted
UPDATE facts SET v = v + 100 WHERE id % 9 = 0 AND id <= 20000;
DELETE FROM facts WHERE id % 11 = 0 AND id <= 20000;
SELECT * FROM probe();

-- a writer that aborted: its pair edits and its facts are in no snapshot
BEGIN;
INSERT INTO membership VALUES (4, 12), (5, 12), (6, 12), (304, 4) ON CONFLICT DO NOTHING;
DELETE FROM membership WHERE dim_a_id IN (8, 9, 10);
INSERT INTO facts SELECT 30000 + i, 1 + i % 320, timestamptz '2026-02-01', 1, 'z' FROM generate_series(1, 50) i;
ROLLBACK;
SELECT * FROM probe();

-- this transaction's own changes are seen by its own reads
BEGIN;
DELETE FROM membership WHERE dim_a_id IN (11, 12, 13);
INSERT INTO membership VALUES (11, 1), (305, 2), (306, 1), (306, 2) ON CONFLICT DO NOTHING;
INSERT INTO facts SELECT 40000 + i, CASE i % 4 WHEN 0 THEN 11 WHEN 1 THEN 305 WHEN 2 THEN 306 ELSE 12 END, timestamptz '2026-02-01', 2, 'w' FROM generate_series(1, 40) i;
SELECT * FROM probe();
ROLLBACK;
SELECT * FROM probe();

-- random rounds of pair edits (members in 0 to 6 keys) and fact writes, each committed, each followed by a read of random keys, once and per pair: every read is md5 equal to the heap
SELECT setseed(0.42);
CREATE TABLE fuzz_result (reads int, scope_reads int, mismatches int);
DO $$
DECLARE
	reads int := 0;
	scope_reads int := 0;
	mism int := 0;
	keys int8[];
	pairs boolean;
	r record;
BEGIN
	FOR i IN 1..60 LOOP
		CASE floor(random() * 7)::int
			WHEN 0 THEN INSERT INTO membership SELECT 1 + floor(random() * 330)::int, 1 + floor(random() * 12)::int FROM generate_series(1, 1 + floor(random() * 6)::int) ON CONFLICT DO NOTHING;
			WHEN 1 THEN DELETE FROM membership WHERE dim_a_id = 1 + floor(random() * 330)::int;
			WHEN 2 THEN DELETE FROM membership WHERE group_id = 1 + floor(random() * 12)::int AND dim_a_id % 5 = floor(random() * 5)::int;
			WHEN 3 THEN WITH d AS (DELETE FROM membership WHERE dim_a_id % 13 = floor(random() * 13)::int RETURNING dim_a_id, group_id)
				INSERT INTO membership SELECT dim_a_id, 1 + (group_id + floor(random() * 11)::int) % 12 FROM d ON CONFLICT DO NOTHING;
			WHEN 4 THEN INSERT INTO facts SELECT 100000 + i * 100 + j, 1 + floor(random() * 330)::int, timestamptz '2026-03-01', j % 7, 'f' FROM generate_series(1, 20) j;
			WHEN 5 THEN UPDATE facts SET v = v + 1 WHERE id % 97 = floor(random() * 97)::int;
			ELSE DELETE FROM facts WHERE id % 101 = floor(random() * 101)::int;
		END CASE;
		COMMIT;
		SELECT array_agg(DISTINCT 1 + floor(random() * 12)::int8) INTO keys FROM generate_series(1, 1 + floor(random() * 6)::int);
		FOREACH pairs IN ARRAY ARRAY[false, true] LOOP
			SELECT * INTO r FROM scope_run(keys_q(keys, pairs));
			reads := reads + 1;
			scope_reads := scope_reads + (r.route IS NOT NULL)::int;
			mism := mism + (NOT r.same)::int;
		END LOOP;
	END LOOP;
	INSERT INTO fuzz_result VALUES (reads, scope_reads, mism);
	COMMIT;
END $$;
SELECT reads, scope_reads >= reads / 4 AS joins_read_by_scope, mismatches FROM fuzz_result;
SELECT sift.col_verify('facts_col', true);

-- the rows of the buffer promoted
SELECT sift.col_promote('facts_col') >= 0 AS promoted;
SELECT * FROM probe();
SELECT sift.col_verify('facts_col', true);

DROP TABLE fuzz_result, probes, facts, membership;
DROP FUNCTION probe(), keys_q(int8[], boolean), scope_run(text);
DROP EXTENSION pg_sift;
