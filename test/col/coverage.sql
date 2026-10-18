-- Runner of the coverage proofs: one sample query of the coverage table, run against the claim of its row.
-- Needs the helpers of test/col/run.sql (coldiff.header, materialize, explain, compare, apply, reset) in the same database.
--
--   expect  claim checked; "<claim> ~ REGEX" or "<claim> !~ REGEX" adds a regex that the plan must (not) match
--   agg     the plan has Custom Scan (SiftAgg) and the result equals the one with every sift plan off
--   plain   the plan has no SiftAgg (falls back to the normal plan) and the result equals the reference
--   scan    the plan has Custom Scan (SiftScan); count: SiftCount; bitmap: a Bitmap Index Scan on a sift index
--   noscan  none of SiftAgg, SiftScan, SiftCount: the normal plan, the same result
--   ok      the result equals the reference (any plan)
--   error:C the statement raises SQLSTATE C; with `avoid` ("name = value"), it runs without error under that setting
--   true    the statement is a boolean self-check (same, in_tx, workers_agree, snapshot below, or a catalog query) that returns true
-- The reference run has sift.enable_agg, enable_scan, enable_count and enable_col off and the btree and bitmap scans off: the plan
-- PostgreSQL has without sift. A query that ranks (sift.score) has no such plan: only its plan and that it runs are checked.
\set ON_ERROR_STOP on
DROP SCHEMA IF EXISTS coverage CASCADE;
CREATE SCHEMA coverage;
CREATE TABLE coverage.results (id text PRIMARY KEY, expect text NOT NULL, ok boolean NOT NULL, kind text NOT NULL, detail text, plan text);

-- the settings of the reference run (the plan without sift) on top of coldiff.apply('off')
CREATE FUNCTION coverage.reference(gucs text[]) RETURNS void LANGUAGE plpgsql AS $$
DECLARE g text;
BEGIN
	PERFORM coldiff.apply('off', gucs);
	FOREACH g IN ARRAY ARRAY['sift.enable_scan', 'sift.enable_count', 'sift.enable_col', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan'] LOOP
		PERFORM set_config(g, 'off', false);
	END LOOP;
END $$;

CREATE FUNCTION coverage.reset(gucs text[] DEFAULT NULL) RETURNS void LANGUAGE plpgsql AS $$
DECLARE g text;
BEGIN
	PERFORM coldiff.reset(gucs);
	FOREACH g IN ARRAY ARRAY['sift.enable_scan', 'sift.enable_count', 'sift.enable_col', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan',
							 'seq_page_cost', 'random_page_cost', 'cpu_tuple_cost'] LOOP
		EXECUTE 'RESET ' || g;
	END LOOP;
END $$;

-- the sift node the plan has, for the report
CREATE FUNCTION coverage.nodes(plan text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT coalesce(string_agg(DISTINCT m[1], ' + '), 'normal plan') FROM regexp_matches(plan, 'Custom Scan \((Sift\w+)\)', 'g') m $$;

CREATE FUNCTION coverage.record(rid text, expect text, ok boolean, kind text, detail text, plan text) RETURNS void LANGUAGE sql AS $$
	INSERT INTO coverage.results VALUES (rid, expect, ok, kind, detail, plan) ON CONFLICT (id) DO UPDATE SET ok = excluded.ok, kind = excluded.kind, detail = excluded.detail, plan = excluded.plan $$;

-- error:CODE rows: the SQLSTATE of the statement (NULL: none)
CREATE FUNCTION coverage.sqlstate(stmt text, gucs text[]) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM coldiff.apply('on', gucs);
	EXECUTE stmt;
	RETURN NULL;
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE;
END $$;

-- ---------- self-checking probes: the query of a `true` row is one of these calls (or any boolean expression) ----------
CREATE FUNCTION coverage.rows_text(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION coverage.plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP r := r || l || E'\n'; END LOOP;
	RETURN r;
END $$;

-- the plan of q has a Custom Scan (SiftAgg) and q returns what it returns with sift.enable_agg off
CREATE FUNCTION coverage.same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text; p text;
BEGIN
	PERFORM set_config('seq_page_cost', '10000', true), set_config('random_page_cost', '10000', true), set_config('sift.enable_agg', 'on', true);
	p := coverage.plan_of(q); a := coverage.rows_text(q);
	PERFORM set_config('sift.enable_agg', 'off', true);
	b := coverage.rows_text(q);
	RETURN p LIKE '%Custom Scan (SiftAgg)%' AND a = b;
END $$;

-- the DML statements run, q is the same as with sift off, then they roll back: the transaction's own uncommitted changes
CREATE FUNCTION coverage.in_tx(stmts text[], q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE s text; ok boolean;
BEGIN
	BEGIN
		FOREACH s IN ARRAY stmts LOOP EXECUTE s; END LOOP;
		ok := coverage.same(q);
		RAISE EXCEPTION 'rollback' USING ERRCODE = 'P0RBK';
	EXCEPTION WHEN SQLSTATE 'P0RBK' THEN NULL;
	END;
	RETURN ok;
END $$;

-- q has the same result at 0, 2 and 4 workers (bit for bit) and a SiftAgg plan with workers planned at 2
CREATE FUNCTION coverage.workers_agree(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE w int; r text[]; p text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true), set_config('parallel_setup_cost', '0', true), set_config('parallel_tuple_cost', '0', true),
		set_config('min_parallel_table_scan_size', '0', true), set_config('seq_page_cost', '10000', true), set_config('random_page_cost', '10000', true);
	FOREACH w IN ARRAY ARRAY[0, 2, 4] LOOP
		PERFORM set_config('max_parallel_workers_per_gather', w::text, true);
		r := r || coverage.rows_text(q);
		IF w = 2 THEN p := coverage.plan_of(q); END IF;
	END LOOP;
	RETURN r[1] = r[2] AND r[2] = r[3] AND p LIKE '%Custom Scan (SiftAgg)%' AND p ~ 'Workers Planned: [1-9]';
END $$;

-- the control query has a SiftAgg plan and none of the queries qs has one: the shape of qs, not the table, keeps the normal plan
CREATE FUNCTION coverage.keeps_normal(control text, qs text[]) RETURNS boolean LANGUAGE sql AS $$
	SELECT coverage.plan_of(control) LIKE '%Custom Scan (SiftAgg)%'
		AND NOT EXISTS (SELECT 1 FROM unnest(qs) q WHERE coverage.plan_of(q) LIKE '%Custom Scan (SiftAgg)%') $$;

-- a SCROLL cursor over the table: FETCH LAST and five FETCH PRIOR return the last rows of the forward scan, in reverse
CREATE FUNCTION coverage.scroll_back(tbl text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE c refcursor; fwd int[]; back int[] := '{}'; r record; i int;
BEGIN
	EXECUTE format('SELECT array_agg(id) FROM (SELECT id FROM %s) x', tbl) INTO fwd;
	OPEN c SCROLL FOR EXECUTE format('SELECT id FROM %s', tbl);
	FETCH LAST FROM c INTO r; back := back || r.id;
	FOR i IN 1..5 LOOP FETCH PRIOR FROM c INTO r; back := back || r.id; END LOOP;
	CLOSE c;
	RETURN back = ARRAY(SELECT fwd[cardinality(fwd) - g] FROM generate_series(0, 5) g);
END $$;

-- a reader in a transaction of the isolation level iso sees the same rows whatever the writer (a second session) does, before and after
-- its commit, with and without the aggregate scan; the writer's statements are DML on the table of q
CREATE FUNCTION coverage.snapshot(iso text, q text, writer text[]) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	cs text := format('dbname=%s user=%s host=%s port=%s', current_database(), session_user, split_part(current_setting('unix_socket_directories'), ',', 1), current_setting('port'));
	wrap text := format('SELECT coverage.rows_text(%L)', q);
	s text; before text; open_on text; open_off text; done_on text; done_off text; p text;
BEGIN
	PERFORM dblink_connect('cv_r', cs), dblink_connect('cv_w', cs);
	PERFORM dblink_exec('cv_r', 'LOAD ''pg_sift'''), dblink_exec('cv_r', 'SET seq_page_cost = 10000'), dblink_exec('cv_r', 'SET random_page_cost = 10000');
	PERFORM dblink_exec('cv_r', 'BEGIN ISOLATION LEVEL ' || iso);
	SELECT x INTO before FROM dblink('cv_r', wrap) t(x text);
	PERFORM dblink_exec('cv_w', 'BEGIN');
	FOREACH s IN ARRAY writer LOOP PERFORM dblink_exec('cv_w', s); END LOOP;
	SELECT x INTO open_on FROM dblink('cv_r', wrap) t(x text);
	PERFORM dblink_exec('cv_r', 'SET sift.enable_agg = off');
	SELECT x INTO open_off FROM dblink('cv_r', wrap) t(x text);
	PERFORM dblink_exec('cv_w', 'COMMIT'), dblink_exec('cv_r', 'SET sift.enable_agg = on');
	SELECT x INTO done_on FROM dblink('cv_r', wrap) t(x text);
	SELECT string_agg(l, E'\n') INTO p FROM dblink('cv_r', format('SELECT coverage.plan_of(%L)', q)) t(l text);
	PERFORM dblink_exec('cv_r', 'SET sift.enable_agg = off');
	SELECT x INTO done_off FROM dblink('cv_r', wrap) t(x text);
	PERFORM dblink_exec('cv_r', 'ROLLBACK'), dblink_disconnect('cv_r'), dblink_disconnect('cv_w');
	RETURN p LIKE '%Custom Scan (SiftAgg)%' AND before = open_on AND before = open_off AND before = done_on AND before = done_off;
EXCEPTION WHEN OTHERS THEN
	PERFORM dblink_disconnect(c) FROM unnest(dblink_get_connections()) c WHERE c IN ('cv_r', 'cv_w');
	RAISE;
END $$;

-- rid: the row of the table; expect: the claim; txt: the query (a corpus file: coldiff.header tags guc/key allowed); avoid: "name = value"
CREATE FUNCTION coverage.check(rid text, expect text, txt text, avoid text DEFAULT NULL) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	h jsonb := coldiff.header(txt); body text := coldiff.expand(h ->> 'body', CASE WHEN h ? 'sets' THEN (string_to_array(h ->> 'sets', ' '))[array_length(string_to_array(h ->> 'sets', ' '), 1)] END);
	stmt text := CASE WHEN h ? 'prepare' THEN format('EXECUTE coverage_p(%s)', coldiff.expand(coalesce(h ->> 'args', ''), NULL))
					  WHEN h ->> 'wrap' = 'subquery' THEN format('SELECT * FROM (%s) coverage_w', body) ELSE body END;
	gucs text[] := ARRAY(SELECT jsonb_array_elements_text(coalesce(h -> 'guc', '[]')));
	keycols int[] := CASE WHEN h ->> 'key' = '-' THEN NULL WHEN coalesce(h ->> 'key', '') = '' THEN '{}'::int[] ELSE string_to_array(h ->> 'key', ' ')::int[] END;
	m text[] := regexp_match(expect, '^(\w+)(?: (!?)~ (.*))?$'); claim text := m[1]; negate boolean := m[2] = '!'; extra text := m[3];
	i int; who text; err_ref text; err_got text; plan text; msg text; got text; want text; has_agg boolean; has_scan boolean; has_count boolean; has_bitmap boolean; ranks boolean; okv boolean;
BEGIN
	IF expect LIKE 'error:%' THEN
		want := substr(expect, 7);
		got := coverage.sqlstate(stmt, gucs);
		PERFORM coverage.reset(gucs);
		IF got IS DISTINCT FROM want THEN
			PERFORM coverage.record(rid, expect, false, 'plan', format('raised %s, claimed %s', coalesce(got, 'nothing'), want), NULL);
			RETURN;
		END IF;
		IF avoid ~ '^[\w.]+ *= *' THEN
			got := coverage.sqlstate(stmt, gucs || avoid);
			PERFORM coverage.reset(gucs);
			IF got IS NOT NULL THEN
				PERFORM coverage.record(rid, expect, false, 'plan', format('still raises %s under "%s"', got, avoid), NULL);
				RETURN;
			END IF;
		END IF;
		PERFORM coverage.record(rid, expect, true, 'ok', NULL, NULL);
		RETURN;
	END IF;

	IF claim = 'true' THEN
		-- the query is a self-check (coverage.same, workers_agree, snapshot, a catalog query): one boolean that must be true
		PERFORM coldiff.apply('on', gucs);
		BEGIN
			EXECUTE stmt INTO okv;
			msg := CASE WHEN okv IS TRUE THEN NULL ELSE 'the query returned ' || coalesce(okv::text, 'NULL') END;
		EXCEPTION WHEN OTHERS THEN msg := 'the query raised ' || SQLSTATE || ': ' || SQLERRM;
		END;
		PERFORM coverage.reset(gucs);
		PERFORM coverage.record(rid, expect, msg IS NULL, CASE WHEN msg IS NULL THEN 'ok' ELSE 'plan' END, msg, NULL);
		RETURN;
	END IF;

	ranks := body ~* 'sift\.score';
	-- a `role = name` setting is for the statements only: the harness itself keeps the privileges of the caller
	FOR i IN 1..coalesce(array_length(gucs, 1), 0) LOOP
		IF gucs[i] ~ '^\s*role\s*=' THEN who := substring(gucs[i] FROM '=\s*(\S+)'); gucs := array_remove(gucs, gucs[i]); EXIT; END IF;
	END LOOP;
	-- the tables are tiny: the scan's start-up cost would keep the btree plans that a claim of a shape is not about
	IF claim IN ('agg', 'plain') AND NOT EXISTS (SELECT 1 FROM unnest(gucs) g WHERE g ~ '^\s*seq_page_cost') THEN
		gucs := gucs || ARRAY['seq_page_cost = 10000', 'random_page_cost = 10000'];
	END IF;
	PERFORM coverage.reference(gucs);
	IF h ? 'prepare' THEN DEALLOCATE ALL; EXECUTE format('PREPARE coverage_p %s AS %s', h ->> 'prepare', body); END IF;
	PERFORM set_config('role', coalesce(who, 'none'), false);
	err_ref := coldiff.materialize(stmt, 'coverage_ref');
	PERFORM set_config('role', 'none', false);
	PERFORM coverage.reset(gucs);
	PERFORM coldiff.apply('on', gucs);
	IF h ? 'prepare' THEN DEALLOCATE ALL; EXECUTE format('PREPARE coverage_p %s AS %s', h ->> 'prepare', body); END IF;
	PERFORM set_config('role', coalesce(who, 'none'), false);
	err_got := coldiff.materialize(stmt, 'coverage_got');
	plan := coldiff.explain(stmt);
	PERFORM set_config('role', 'none', false);
	PERFORM coverage.reset(gucs);
	IF h ? 'prepare' THEN DEALLOCATE ALL; END IF;
	has_agg := plan LIKE '%Custom Scan (SiftAgg)%';
	has_scan := plan LIKE '%Custom Scan (SiftScan)%';
	has_count := plan LIKE '%Custom Scan (SiftCount)%';
	has_bitmap := plan ~ 'Bitmap Index Scan on \w*';

	-- the result first: a wrong result fails whatever the claim
	IF err_got IS NOT NULL OR (err_ref IS NOT NULL AND NOT ranks) THEN
		msg := format('the query raised: off %s; on %s', coalesce(err_ref, 'no error'), coalesce(err_got, 'no error'));
		IF left(err_ref, 5) IS NOT DISTINCT FROM left(err_got, 5) THEN msg := NULL; END IF;
	ELSIF NOT ranks THEN
		msg := coldiff.compare('coverage_ref', 'coverage_got', keycols, 1e-9, 1e-12);
	END IF;
	IF msg IS NOT NULL THEN
		PERFORM coverage.record(rid, expect, false, 'result', msg, plan);
		RETURN;
	END IF;

	msg := CASE claim
		WHEN 'agg' THEN CASE WHEN NOT has_agg THEN 'the plan has no Custom Scan (SiftAgg)' END
		WHEN 'plain' THEN CASE WHEN has_agg THEN 'the plan has Custom Scan (SiftAgg)' END
		WHEN 'scan' THEN CASE WHEN NOT has_scan THEN 'the plan has no Custom Scan (SiftScan)' END
		WHEN 'count' THEN CASE WHEN NOT has_count THEN 'the plan has no Custom Scan (SiftCount)' END
		WHEN 'bitmap' THEN CASE WHEN NOT has_bitmap THEN 'the plan has no Bitmap Index Scan' END
		WHEN 'noscan' THEN CASE WHEN has_agg OR has_scan OR has_count THEN 'the plan has ' || coverage.nodes(plan) END
		WHEN 'ok' THEN NULL
		ELSE 'unknown claim ' || expect END;
	IF msg IS NULL AND extra IS NOT NULL THEN
		IF (plan ~ extra) = negate THEN msg := format('the plan %s "%s"', CASE WHEN negate THEN 'matches' ELSE 'does not match' END, extra); END IF;
	END IF;
	PERFORM coverage.record(rid, expect, msg IS NULL, CASE WHEN msg IS NULL THEN 'ok' ELSE 'plan' END, msg, plan);
	DROP TABLE IF EXISTS pg_temp.coverage_ref, pg_temp.coverage_got;
END $$;
