-- A statement planned while an index was in service keeps its plan, which names the index, when the index leaves service without the plan being invalidated (another backend
-- recorded the damage while this transaction holds the locks of the plan, so it takes no invalidation before it runs the plan again). The node of SiftAggV6 that opens the index is
-- refused with SQLSTATE 40001, the one a client retries, and the plans that name the index are dropped, so the retry is planned without it and gives the rows of PostgreSQL's plan. A
-- node must not read the index under the plan of before. Each node has a table of its own: the refusal drops every plan of a table. Needs dblink and pg_sift_test.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION IF NOT EXISTS dblink;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
SET client_min_messages = error;
SET plan_cache_mode = force_generic_plan;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;
CREATE FUNCTION engine(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text[] := '{}';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan' THEN r := r || substring(l from 'Custom Scan \(([A-Za-z0-9]+)\)'); END IF;
	END LOOP;
	RETURN coalesce(nullif(array_to_string(r, ','), ''), 'normal');
END $$;
CREATE FUNCTION answer(q text, sift_on bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', sift_on::text, true);
	EXECUTE format('CREATE TEMP TABLE got ON COMMIT DROP AS %s', q);
	SELECT md5(coalesce(string_agg(g::text, '|' ORDER BY g::text), '')) INTO r FROM got g;
	DROP TABLE got;
	RETURN r;
END $$;

CREATE TABLE ev_agg (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
CREATE TABLE ev_top (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
CREATE TABLE ev_win (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev_agg SELECT i, i % 5100, i % 11 FROM generate_series(1, 40000) i;
INSERT INTO ev_top SELECT * FROM ev_agg;
INSERT INTO ev_win SELECT * FROM ev_agg;
CREATE INDEX ev_agg_col ON ev_agg USING sift_col (id, k, v);
CREATE INDEX ev_top_col ON ev_top USING sift_col (id, k, v);
CREATE INDEX ev_win_col ON ev_win USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) ev_agg, ev_top, ev_win;

PREPARE agg(int) AS SELECT v, count(*), sum(k) FROM ev_agg WHERE k < $1 GROUP BY v;
PREPARE top_n(int) AS SELECT id FROM ev_top ORDER BY k DESC, id LIMIT $1;
PREPARE win(int) AS SELECT count(*) FROM (SELECT row_number() OVER (PARTITION BY v ORDER BY k, id) rn FROM ev_win WHERE k < 3000) s WHERE rn <= $1;
SELECT engine($$EXECUTE agg(1000)$$) AS agg_plan, engine($$EXECUTE top_n(5)$$) AS top_n_plan, engine($$EXECUTE win(3)$$) AS window_plan;

BEGIN;
SELECT sift_helpers.statement_outcome($$EXECUTE agg(1000)$$, ': ') AS agg, sift_helpers.statement_outcome($$EXECUTE top_n(5)$$, ': ') AS top_n, sift_helpers.statement_outcome($$EXECUTE win(3)$$, ': ') AS window;
SELECT dblink_connect('damage', format('dbname=%s user=%s', current_database(), current_user));
SELECT r AS damage FROM unnest(ARRAY['ev_agg_col', 'ev_top_col', 'ev_win_col']) i,
	LATERAL dblink('damage', format($q$SELECT sift_helpers.statement_outcome($$SELECT sift._test_raise_extending(%L, 'index', false)$$, ': ')$q$, i)) t(r text) ORDER BY i;
SELECT dblink_disconnect('damage');
SELECT sift_helpers.statement_outcome($$EXECUTE agg(1000)$$, ': ') AS agg_prepared;
SELECT sift_helpers.statement_outcome($$EXECUTE top_n(5)$$, ': ') AS top_n_prepared;
SELECT sift_helpers.statement_outcome($$EXECUTE win(3)$$, ': ') AS window_prepared;
COMMIT;
SELECT index FROM sift.quarantine ORDER BY 1;
SELECT sift_helpers.statement_outcome($$EXECUTE agg(1000)$$, ': ') AS agg_planned_again, sift_helpers.statement_outcome($$EXECUTE top_n(5)$$, ': ') AS top_n_planned_again, sift_helpers.statement_outcome($$EXECUTE win(3)$$, ': ') AS window_planned_again;
SELECT engine($$EXECUTE agg(1000)$$) AS agg_plan, engine($$EXECUTE top_n(5)$$) AS top_n_plan, engine($$EXECUTE win(3)$$) AS window_plan;
SELECT answer($$EXECUTE agg(1000)$$, true) = answer($$EXECUTE agg(1000)$$, false) AS same_agg,
	answer($$EXECUTE top_n(5)$$, true) = answer($$EXECUTE top_n(5)$$, false) AS same_top_n,
	answer($$EXECUTE win(3)$$, true) = answer($$EXECUTE win(3)$$, false) AS same_window;

DEALLOCATE ALL;
DROP TABLE ev_agg, ev_top, ev_win;
DROP FUNCTION engine(text), answer(text, bool);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION dblink;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
