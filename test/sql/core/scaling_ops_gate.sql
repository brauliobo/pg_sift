-- The scaling gate of the SQL shapes that matter at scale (scripts/check-scaling-ops.sh): each shape is built at 2,000 and at 16,000 rows (8x) and its buffers are compared. A clock is never
-- read: the shared blocks a statement touches (hit + read, the same whether the cache is cold or warm), the temp blocks it writes (none) and the nodes of its plan are the same on every run.
-- A row is `PASS | shape | node_ok | temp | ratio_ok`: the plan has the node the shape is about, nothing spilled, and the buffers of 16,000 rows are at most the ceiling of the shape (8 blocks
-- of slack) times those of 2,000. A shape whose code is not there yet says PENDING and the lane that makes it meet its ceiling: it must stay over it until that lane lands, then
-- the row says STALE and the mark is removed (the same two-sided ratchet as scripts/complexity.allow).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET work_mem = '32MB';
SET sift.async_maintenance = off;
SET TimeZone = 'UTC';
-- the library of the testing build creates the indexes of format major 1 by default; the shapes are of format major 2, which the release library creates
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

-- the blocks, the nodes and the temp blocks of one statement; a statement that writes is rolled back by the exception, and the variables keep what was measured
CREATE FUNCTION pg_temp.measure(q text, settings text[], OUT blks bigint, OUT temp_blks bigint, OUT nodes text[]) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	s text;
BEGIN
	BEGIN
		FOREACH s IN ARRAY settings LOOP
			EXECUTE 'SET LOCAL ' || s;
		END LOOP;
		EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
		blks := coalesce((j->0->'Plan'->>'Shared Hit Blocks')::bigint, 0) + coalesce((j->0->'Plan'->>'Shared Read Blocks')::bigint, 0);
		WITH RECURSIVE n(p) AS (SELECT j->0->'Plan' UNION ALL SELECT c FROM n, jsonb_array_elements(coalesce(p->'Plans', '[]')) c)
		SELECT coalesce(sum(coalesce((p->>'Temp Read Blocks')::bigint, 0) + coalesce((p->>'Temp Written Blocks')::bigint, 0)), 0),
			   array_agg(DISTINCT coalesce(p->>'Custom Plan Provider', p->>'Node Type'))
		INTO temp_blks, nodes FROM n;
		RAISE EXCEPTION 'rolled back' USING ERRCODE = 'P0001';
	EXCEPTION WHEN raise_exception THEN
		NULL;
	END;
END $$;

-- one row of the gate: the second run of each statement is the one measured (the first fills the caches of the backend)
CREATE FUNCTION pg_temp.gate(shape text, q_small text, q_big text, expect text, max_ratio float8, pending text DEFAULT NULL,
							 settings_small text[] DEFAULT '{}', settings_big text[] DEFAULT '{}') RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a record;
	b record;
	ok boolean;
	node_ok boolean;
	ratio_ok boolean;
BEGIN
	PERFORM pg_temp.measure(q_small, settings_small);
	PERFORM pg_temp.measure(q_big, settings_big);
	SELECT * INTO a FROM pg_temp.measure(q_small, settings_small);
	SELECT * INTO b FROM pg_temp.measure(q_big, settings_big);
	node_ok := expect = ANY (a.nodes) AND expect = ANY (b.nodes);
	ratio_ok := b.blks <= ceil(a.blks * max_ratio) + 8;
	ok := node_ok AND ratio_ok AND a.temp_blks = 0 AND b.temp_blks = 0;
	RETURN format('%s | %s | node_ok=%s | temp=%s | ratio_ok=%s%s',
				  CASE WHEN pending IS NULL THEN CASE WHEN ok THEN 'PASS' ELSE 'FAIL' END ELSE CASE WHEN ok THEN 'STALE' ELSE 'PENDING' END END,
				  shape, left(node_ok::text, 1), CASE WHEN a.temp_blks + b.temp_blks = 0 THEN '0' ELSE 'spill' END, left(ratio_ok::text, 1),
				  CASE WHEN pending IS NULL THEN '' ELSE ' | pending ' || pending END);
END $$;

-- 1: a point lookup of a sift_col index
CREATE FUNCTION pg_temp.mk_point(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE pt%sk (id int NOT NULL, grp int NOT NULL, ts timestamptz NOT NULL, val float8 NOT NULL) WITH (autovacuum_enabled = false)', n / 1000);
	EXECUTE format('INSERT INTO pt%sk SELECT g, g %% 50, timestamptz ''2025-01-01'' + g * interval ''1 minute'', g FROM generate_series(1, %s) g', n / 1000, n);
	EXECUTE format('CREATE INDEX pt%sk_c ON pt%sk USING sift_col (id, grp, ts, val) WITH (key_order = id)', n / 1000, n / 1000);
	EXECUTE format('ANALYZE pt%sk', n / 1000);
END $$;

-- 2, 3, 4: the scope of a parent over its entities (16,000 and 128,000 rows: the planner reads the scope below about 9,000 rows as a join, or pushes the join into the scan): a count, and the newest 20 rows, clean and with 5% of the rows inserted and deleted and two entities moved to the parent
CREATE FUNCTION pg_temp.mk_scope(n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE k int := n / 1000;
BEGIN
	EXECUTE format('CREATE TABLE ep%sk (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false)', k);
	EXECUTE format('INSERT INTO ep%sk SELECT e, 1 + e %% 50 FROM generate_series(1, 200) e', k);
	EXECUTE format('CREATE TABLE ev%sk (id int PRIMARY KEY, entity_id int NOT NULL, event_date date NOT NULL, amount numeric(12,2) NOT NULL, status int NOT NULL) WITH (autovacuum_enabled = false)', k);
	EXECUTE format('INSERT INTO ev%sk SELECT i, 1 + (i * 7919) %% 200, DATE ''2024-01-01'' + i %% 500, (i %% 700) / 4.0, i %% 5 FROM generate_series(1, %s) i', k, n);
	EXECUTE format('CREATE INDEX ev%sk_s ON ev%sk USING sift_col (id, entity_id, event_date, amount, status) WITH (key_order = ''entity_id, event_date'', '
				   'scope = ''parent_id: entity_id -> ep%sk(entity_id, parent_id)'', track_changes = ''ctid'')', k, k, k);
	EXECUTE format('ANALYZE ev%sk', k);
	EXECUTE format('ANALYZE ep%sk', k);
END $$;

CREATE FUNCTION pg_temp.dirty_scope(n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE k int := n / 1000;
BEGIN
	EXECUTE format('INSERT INTO ev%sk SELECT %s + i, 1 + (i * 31) %% 200, DATE ''2024-01-01'' + i %% 500, 1.5, 1 FROM generate_series(1, %s) i', k, n, n / 20);
	EXECUTE format('DELETE FROM ev%sk WHERE id %% 20 = 0', k);
	EXECUTE format('UPDATE ep%sk SET parent_id = 42 WHERE entity_id IN (3, 4)', k);
END $$;

CREATE FUNCTION pg_temp.scope_count(k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT count(*), sum(e.amount) FROM ev%sk e JOIN ep%sk ep ON ep.entity_id = e.entity_id WHERE ep.parent_id = 42', k, k) $$;
CREATE FUNCTION pg_temp.scope_top(k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT e.id, e.event_date, e.amount FROM ev%sk e JOIN ep%sk ep ON ep.entity_id = e.entity_id WHERE ep.parent_id = 42 ORDER BY e.event_date DESC, e.id DESC LIMIT 20', k, k) $$;

-- 5, 6: a bulk delete and a column update of a sift_table that touch an eighth of the rows
CREATE FUNCTION pg_temp.mk_table(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE tb%sk (id int PRIMARY KEY, g int, v int) USING sift_table', n / 1000);
	EXECUTE format('INSERT INTO tb%sk SELECT i, i %% 50, i FROM generate_series(1, %s) i', n / 1000, n);
	EXECUTE format('ANALYZE tb%sk', n / 1000);
END $$;

-- 7: a sum grouped over the rows of a list of ids as long as an eighth of the table
CREATE FUNCTION pg_temp.inlist(k int, n int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT grp, sum(val) FROM pt%sk WHERE id = ANY (ARRAY[%s]) GROUP BY grp', k, (SELECT string_agg((g * 8)::text, ',') FROM generate_series(1, n / 8) g)) $$;

-- 8: the facts joined to the intervals of a dimension by a range, the dimension 8x as long as the facts
CREATE FUNCTION pg_temp.mk_star(n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE k int := n / 1000;
BEGIN
	EXECUTE format('CREATE TABLE fx%sk (id int, ts int NOT NULL, val int NOT NULL) WITH (autovacuum_enabled = false)', k);
	EXECUTE format('INSERT INTO fx%sk SELECT i, i, i %% 100 FROM generate_series(1, %s) i', k, n);
	EXECUTE format('CREATE INDEX fx%sk_c ON fx%sk USING sift_col (id, ts, val)', k, k);
	EXECUTE format('CREATE TABLE dm%sk (cat int NOT NULL, lo int NOT NULL, hi int NOT NULL)', k);
	EXECUTE format('INSERT INTO dm%sk SELECT i %% 5, (i - 1) * %s, i * %s FROM generate_series(1, %s) i', k, n / (n / 125), n / (n / 125), n / 125);
	EXECUTE format('ANALYZE fx%sk', k);
	EXECUTE format('ANALYZE dm%sk', k);
END $$;

-- 9: a BM25 index over documents of 12 words of 200, the query of 16 terms and of 128
CREATE FUNCTION pg_temp.mk_docs(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE bm%sk (id int, body text) WITH (autovacuum_enabled = false)', n / 1000);
	EXECUTE format('INSERT INTO bm%sk SELECT g, (SELECT string_agg(''w'' || (get_byte(decode(md5(g || '':'' || k), ''hex''), 0) %% 200), '' '') FROM generate_series(1, 12) k) FROM generate_series(1, %s) g', n / 1000, n);
	EXECUTE format('CREATE INDEX bm%sk_i ON bm%sk USING sift (body)', n / 1000, n / 1000);
	EXECUTE format('ANALYZE bm%sk', n / 1000);
END $$;
CREATE FUNCTION pg_temp.terms(k int, n int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT id FROM bm%sk WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 20', k, (SELECT string_agg('w' || g, ' ') FROM generate_series(0, n - 1) g)) $$;

SELECT pg_temp.mk_point(2000), pg_temp.mk_point(16000), pg_temp.mk_scope(16000), pg_temp.mk_scope(128000), pg_temp.mk_table(2000), pg_temp.mk_table(16000),
       pg_temp.mk_star(2000), pg_temp.mk_star(16000), pg_temp.mk_docs(2000), pg_temp.mk_docs(16000) \gset

-- the shapes about an index node are measured without the sequential scan: at 2,000 rows the cost of a plan prices it cheaper, and the gate is about the blocks the index reads
SELECT pg_temp.gate('point lookup', 'SELECT val FROM pt2k WHERE id = 1234', 'SELECT val FROM pt16k WHERE id = 12340', 'Index Scan', 1.8, NULL,
					'{"enable_seqscan = off"}', '{"enable_seqscan = off"}');
SELECT pg_temp.gate('scope count, clean', pg_temp.scope_count(16), pg_temp.scope_count(128), 'SiftAggV6Scope', 1.8);
SELECT pg_temp.dirty_scope(16000), pg_temp.dirty_scope(128000) \gset
SELECT pg_temp.gate('scope count, dirty', pg_temp.scope_count(16), pg_temp.scope_count(128), 'SiftAggV6Scope', 1.8);
SELECT pg_temp.gate('scope top 20, dirty', pg_temp.scope_top(16), pg_temp.scope_top(128), 'SiftAggV6Scope', 2.0, 'Spec 66 #3: dirty ring and added slab (63D-Scope)');
SELECT pg_temp.gate('bulk delete', 'DELETE FROM tb2k WHERE id % 8 = 0', 'DELETE FROM tb16k WHERE id % 8 = 0', 'ModifyTable', 10.5);
SELECT pg_temp.gate('column update', 'UPDATE tb2k SET v = v + 1 WHERE id % 8 = 0', 'UPDATE tb16k SET v = v + 1 WHERE id % 8 = 0', 'ModifyTable', 10.5);
SELECT pg_temp.gate('in list', pg_temp.inlist(2, 2000), pg_temp.inlist(16, 16000), 'SiftAggV6', 10.0, NULL, '{"sift.agg_force = on"}', '{"sift.agg_force = on"}');
SELECT pg_temp.gate('star range join', 'SELECT d.cat, count(*), sum(f.val) FROM fx2k f JOIN dm2k d ON f.ts >= d.lo AND f.ts < d.hi GROUP BY d.cat',
					'SELECT d.cat, count(*), sum(f.val) FROM fx16k f JOIN dm16k d ON f.ts >= d.lo AND f.ts < d.hi GROUP BY d.cat', 'SiftAggV6', 11.5, NULL,
					'{"sift.agg_force = on"}', '{"sift.agg_force = on"}');
SELECT pg_temp.gate('bm25 terms', pg_temp.terms(2, 16), pg_temp.terms(16, 128), 'SiftScan', 11.5);

DROP TABLE pt2k, pt16k, ev16k, ev128k, ep16k, ep128k, tb2k, tb16k, fx2k, fx16k, dm2k, dm16k, bm2k, bm16k;
DROP EXTENSION pg_sift;
