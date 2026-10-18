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

-- rid: the row of the table; expect: the claim; txt: the query (a corpus file: coldiff.header tags guc/key allowed); avoid: "name = value"
CREATE FUNCTION coverage.check(rid text, expect text, txt text, avoid text DEFAULT NULL) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	h jsonb := coldiff.header(txt); body text := coldiff.expand(h ->> 'body', CASE WHEN h ? 'sets' THEN (string_to_array(h ->> 'sets', ' '))[array_length(string_to_array(h ->> 'sets', ' '), 1)] END);
	stmt text := CASE WHEN h ? 'prepare' THEN format('EXECUTE coverage_p(%s)', coldiff.expand(coalesce(h ->> 'args', ''), NULL))
					  WHEN h ->> 'wrap' = 'subquery' THEN format('SELECT * FROM (%s) coverage_w', body) ELSE body END;
	gucs text[] := ARRAY(SELECT jsonb_array_elements_text(coalesce(h -> 'guc', '[]')));
	keycols int[] := CASE WHEN h ->> 'key' = '-' THEN NULL WHEN coalesce(h ->> 'key', '') = '' THEN '{}'::int[] ELSE string_to_array(h ->> 'key', ' ')::int[] END;
	m text[] := regexp_match(expect, '^(\w+)(?: (!?)~ (.*))?$'); claim text := m[1]; negate boolean := m[2] = '!'; extra text := m[3];
	i int; who text; err_ref text; err_got text; plan text; msg text; got text; want text; has_agg boolean; has_scan boolean; has_count boolean; has_bitmap boolean; ranks boolean;
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
