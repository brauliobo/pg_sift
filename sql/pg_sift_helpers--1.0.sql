\echo Use "CREATE EXTENSION pg_sift_helpers" to load this file. \quit

-- The SQL helpers the regression tests share (test/sql). SQL only: it needs no test build of the library, so the tests that use it also run on the release library
-- (scripts/release-library-check.sh). Never installed by a normal make install.

-- The tests that run as a role without rights (row security, grants) call the helpers too
GRANT USAGE ON SCHEMA sift_helpers TO PUBLIC;

-- rows_agg: the helper that tests of test/sql defined themselves (a_rows, b_rows, chars_rows, gn_rows, j_rows, k_rows, long_rows, mk_rows, p_rows, pt_rows, top_rows, wd_rows)
CREATE FUNCTION sift_helpers.rows_agg(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- agg_mode: the helper that tests of test/sql defined themselves (cd_mode, cl_mode, ct_mode, hc_mode, ow_mode, pw_mode, tf_mode, tt_mode, tv_mode)
CREATE FUNCTION sift_helpers.agg_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- margin_note: with sift_helpers.margin = on, the price of the scan and of the normal plan of each arm of q (scripts/plan-margin.sh --tests); the plan helpers below call it
CREATE FUNCTION sift_helpers.margin_note(q text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
	IF coalesce(current_setting('sift_helpers.margin', true), 'off') <> 'on' THEN RETURN; END IF;
	FOR r IN SELECT arm, scan_cost, normal_cost FROM sift.explain_why(q) WHERE scan_cost < 'Infinity' AND normal_cost < 'Infinity' LOOP
		RAISE WARNING 'plan-margin: % scan % normal % gap %', r.arm, round(r.scan_cost::numeric, 1), round(r.normal_cost::numeric, 1),
			round((100 * abs(r.scan_cost - r.normal_cost) / greatest(r.scan_cost, r.normal_cost))::numeric, 0)::text || '%';
	END LOOP;
EXCEPTION WHEN OTHERS THEN NULL;
END $$;

-- plan_is_agg_scan: the helper that tests of test/sql defined themselves (pushed)
CREATE FUNCTION sift_helpers.plan_is_agg_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false;
BEGIN
	PERFORM sift_helpers.margin_note(q);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan := true; END IF;
		IF l ~ '(SiftAggRows|GroupAggregate|HashAggregate)' THEN RETURN false; END IF;
	END LOOP;
	RETURN scan;
END $$;

-- agg_mode_workers: the helper that tests of test/sql defined themselves (ev_mode, iv_mode, nm_mode, ta_mode, tb_mode, tc_mode, tx_mode)
CREATE FUNCTION sift_helpers.agg_mode_workers(pushed bool, workers int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', CASE WHEN pushed THEN workers ELSE 0 END::text, true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
END $$;

-- differences_agg: the helper that tests of test/sql defined themselves (differences)
CREATE FUNCTION sift_helpers.differences_agg(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- plan_has_agg_scan: the helper that tests of test/sql defined themselves (a_pushed, d_pushed, j_pushed, k_pushed, t_pushed)
CREATE FUNCTION sift_helpers.plan_has_agg_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM sift_helpers.margin_note(q);
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- agg_check_outcome: the helper that 6 tests of test/sql defined themselves (chk)
CREATE FUNCTION sift_helpers.agg_check_outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; l text; pushed boolean := false; e_on text; e_off text;
BEGIN
	SET sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP IF l ~ 'Custom Scan \(SiftAgg\)' THEN pushed := true; END IF; END LOOP;
	BEGIN
		EXECUTE 'CREATE TEMP TABLE r_on AS ' || q;
	EXCEPTION WHEN OTHERS THEN e_on := SQLERRM; END;
	SET sift.enable_agg = off;
	SET enable_sort = off;
	BEGIN
		EXECUTE 'CREATE TEMP TABLE r_off AS ' || q;
	EXCEPTION WHEN OTHERS THEN e_off := SQLERRM; END;
	RESET sift.enable_agg;
	RESET enable_sort;
	IF e_on IS NOT NULL OR e_off IS NOT NULL THEN
		DROP TABLE IF EXISTS r_on, r_off;
		RETURN CASE WHEN e_on IS NOT DISTINCT FROM e_off THEN 'error: ' || e_on ELSE 'DIFF on=' || coalesce(e_on, 'ok') || ' off=' || coalesce(e_off, 'ok') END;
	END IF;
	SELECT count(*) INTO a FROM ((SELECT d::text FROM r_on d EXCEPT ALL SELECT d::text FROM r_off d)
								  UNION ALL (SELECT d::text FROM r_off d EXCEPT ALL SELECT d::text FROM r_on d)) x;
	DROP TABLE r_on, r_off;
	RETURN (CASE WHEN pushed THEN 'scan ' ELSE 'plain ' END) || CASE WHEN a = 0 THEN 'same' ELSE 'DIFF ' || a END;
END $$;

-- agg_equals_normal: the helper that 5 tests of test/sql defined themselves (same)
CREATE FUNCTION sift_helpers.agg_equals_normal(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	DROP TABLE r_on, r_off;
	RETURN a = 0;
END $$;

-- agg_equals_normal_nonempty: the helper that 4 tests of test/sql defined themselves (same)
CREATE FUNCTION sift_helpers.agg_equals_normal_nonempty(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0;
END $$;

-- agg_equals_normal_all: the helper that 3 tests of test/sql defined themselves (same)
CREATE FUNCTION sift_helpers.agg_equals_normal_all(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN d = 0 AND n > 0;
END $$;

-- differences_agg_temp: the helper that 4 tests of test/sql defined themselves (s_diff)
CREATE FUNCTION sift_helpers.differences_agg_temp(qq text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE b bigint;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'CREATE TEMP TABLE s_a ON COMMIT DROP AS ' || qq;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'CREATE TEMP TABLE s_b ON COMMIT DROP AS ' || qq;
	EXECUTE 'SELECT count(*) FROM ((SELECT * FROM s_a EXCEPT ALL SELECT * FROM s_b) UNION ALL (SELECT * FROM s_b EXCEPT ALL SELECT * FROM s_a)) d' INTO b;
	DROP TABLE s_a, s_b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN b;
END $$;

-- agg_rows_and_differences: the helper that 5 tests of test/sql defined themselves (same)
CREATE FUNCTION sift_helpers.agg_rows_and_differences(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM (TABLE r_on EXCEPT TABLE r_off) x) + (SELECT count(*) FROM (TABLE r_off EXCEPT TABLE r_on) x);
	RETURN NEXT;
END $$;

-- agg_arms_in_one_pass: the helper that 5 tests of test/sql defined themselves (arms_shared)
CREATE FUNCTION sift_helpers.agg_arms_in_one_pass(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Arms In One Pass: ' THEN n := greatest(n, substr(l, strpos(l, ': ') + 2)::int); END IF;
	END LOOP;
	RETURN n;
END $$;

-- agg_heap_rereads: the helper that 4 tests of test/sql defined themselves (reread)
CREATE FUNCTION sift_helpers.agg_heap_rereads(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ '(\d+) re-read from the heap' THEN n := substring(l from '(\d+) re-read from the heap')::bigint; END IF;
	END LOOP;
	RETURN n;
EXCEPTION WHEN others THEN RETURN -1;
END $$;

-- agg_units_read: the helper that 4 tests of test/sql defined themselves (units_of)
CREATE FUNCTION sift_helpers.agg_units_read(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; read text; pruned text := '0';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l ~ 'Units: (\d+)' THEN read := substring(l from 'Units: (\d+)'); END IF;
		IF l ~ '(\d+) pruned' THEN pruned := substring(l from '(\d+) pruned'); END IF;
	END LOOP;
	RETURN CASE WHEN r = 'normal' THEN r ELSE format('%s read %s pruned %s', r, read, pruned) END;
END $$;

-- plan_text: the helper that 4 tests of test/sql defined themselves (pp_plan, ps_plan, rl_plan, sf_plan)
CREATE FUNCTION sift_helpers.plan_text(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	PERFORM sift_helpers.margin_note(q);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;

-- plan_mentions_siftagg: the helper that 4 tests of test/sql defined themselves (chars_pushed, im_pushed, long_pushed, wd_pushed)
CREATE FUNCTION sift_helpers.plan_mentions_siftagg(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM sift_helpers.margin_note(q);
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- plan_mode_workers: the helper that 3 tests of test/sql defined themselves (mode_of)
CREATE FUNCTION sift_helpers.plan_mode_workers(q text, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; par bool := false; agg bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		par := par OR l LIKE '%Workers Planned: %';
		agg := agg OR l LIKE '%Custom Scan (SiftAgg%';
	END LOOP;
	RETURN CASE WHEN agg AND par THEN 'parallel' WHEN agg THEN 'serial' ELSE 'none' END;
END $$;

-- plan_mode_two_workers: the helper that 3 tests of test/sql defined themselves (p_mode)
CREATE FUNCTION sift_helpers.plan_mode_two_workers(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; par bool := false; agg bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		par := par OR l LIKE '%Workers Planned: 2%';
		agg := agg OR l LIKE '%Custom Scan (SiftAgg)%';
	END LOOP;
	RETURN CASE WHEN agg AND par THEN 'parallel' WHEN agg THEN 'serial' ELSE 'none' END;
END $$;

-- rows_agg_four_scans_or_no_rows: the helper that 6 tests of test/sql defined themselves (au_rows, ca_rows, cp_rows, kf_rows, kr_rows, sp_rows)
CREATE FUNCTION sift_helpers.rows_agg_four_scans_or_no_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- count_agg_pushdown_set: the helper that 4 tests of test/sql defined themselves (count_of)
CREATE FUNCTION sift_helpers.count_agg_pushdown_set(q text, scan bool) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SET LOCAL sift.enable_agg = %s', CASE WHEN scan THEN 'on' ELSE 'off' END);
	EXECUTE q INTO n;
	RETURN n;
END $$;

-- plan_siftagg_or_normal: the helper that 4 tests of test/sql defined themselves (plan_of, shape_of)
CREATE FUNCTION sift_helpers.plan_siftagg_or_normal(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	PERFORM sift_helpers.margin_note(q);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
END $$;

-- rows_agg_four_scans_error_text: the helper that 4 tests of test/sql defined themselves (kp_rows, kt_rows, ss_rows, tk_rows)
CREATE FUNCTION sift_helpers.rows_agg_four_scans_error_text(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

-- plan_siftagg_nodes: the helper that 3 tests of test/sql defined themselves (scans_of)
CREATE FUNCTION sift_helpers.plan_siftagg_nodes(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN n := n + 1; END IF;
	END LOOP;
	RETURN n;
END $$;

-- plan_siftagg_nodes_cached_tables: the helper that 3 tests of test/sql defined themselves (cached, nodes)
CREATE FUNCTION sift_helpers.plan_siftagg_nodes_cached_tables(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; nodes int := 0; kept int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
		IF l ~ '(\d+) cached tables' THEN kept := kept + substring(l from '(\d+) cached tables')::int; END IF;
	END LOOP;
	RETURN format('%s SiftAgg nodes, %s cached tables', nodes, kept);
END $$;

-- agg_events_parts_arms_query: the helper that 3 tests of test/sql defined themselves (arms)
CREATE FUNCTION sift_helpers.agg_events_parts_arms_query() RETURNS text LANGUAGE sql AS $$
	SELECT $q$(SELECT e.id, e.parent_id, e.kind, e.ts, o.cat AS category, avg(o.score) AS score, count(*) AS n
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts, o.cat
	           UNION ALL
	           SELECT e.id, e.parent_id, e.kind, e.ts, NULL::text, avg(o.score), count(*)
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts) m$q$
$$;

-- plan_heap_rereads_forced_on: the helper that 3 tests of test/sql defined themselves (reread)
CREATE FUNCTION sift_helpers.plan_heap_rereads_forced_on(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ '(\d+) re-read from the heap' THEN n := substring(l from '(\d+) re-read from the heap')::bigint; END IF;
	END LOOP;
	RETURN n;
END $$;

-- plan_mentions_siftagg_regex: the helper that 3 tests of test/sql defined themselves (s_scan)
CREATE FUNCTION sift_helpers.plan_mentions_siftagg_regex(qq text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM sift_helpers.margin_note(qq);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || qq LOOP
		IF l ~ 'SiftAgg' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- plan_mentions_siftagg_forced: the helper that 3 tests of test/sql defined themselves (scanned)
CREATE FUNCTION sift_helpers.plan_mentions_siftagg_forced(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET LOCAL sift.agg_force = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- rows_agg_preagg_and_pushdown: the helper that 3 tests of test/sql defined themselves (cc_rows, pa_rows, pm_rows)
CREATE FUNCTION sift_helpers.rows_agg_preagg_and_pushdown(q text, pre bool, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_preagg', pre::text, true);
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- index_key_cut_reloption: the helper that 3 tests of test/sql defined themselves (kopt)
CREATE FUNCTION sift_helpers.index_key_cut_reloption(i regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = i AND o ~ '^key_cut'), 'none') $$;

-- plan_units_lines: the helper that 3 tests of test/sql defined themselves (kunits, nunits)
CREATE FUNCTION sift_helpers.plan_units_lines(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; u text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Units: %' THEN u := u || ' ' || trim(l); END IF;
	END LOOP;
	RETURN u;
END $$;

-- words_from_seed_list: the helper that 3 tests of test/sql defined themselves (cs_words, ct_words)
CREATE FUNCTION sift_helpers.words_from_seed_list(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg((ARRAY['fast','late','damaged','refund','gift','invoice','exchange','missing','perfect','broken','express','pickup',
							 'thanks','wrong','size','color','cable','wire','box','label'])[1 + (hashint4(seed * 31 + w) & 2147483647) % 20], ' ')
	FROM generate_series(1, 3 + seed % 5) w $$;

-- collation_for_text_order: the helper that 3 tests of test/sql defined themselves (tb_coll, tc_coll, tx_coll)
CREATE FUNCTION sift_helpers.collation_for_text_order() RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT collname FROM pg_collation WHERE collname IN ('en_US.utf8', 'und-x-icu') ORDER BY collname LIMIT 1), 'C') $$;

-- plan_limit_hint: the helper that 3 tests of test/sql defined themselves (limit_hint)
CREATE FUNCTION sift_helpers.plan_limit_hint(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Limit Hint: ' THEN RETURN substring(l from 'Limit Hint: (\d+)')::int; END IF;
	END LOOP;
	RETURN 0;
END $$;

-- rows_as_jsonb: the helper that 3 tests of test/sql defined themselves (rr_rows, sf_rows, ss_rows)
CREATE FUNCTION sift_helpers.rows_as_jsonb(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t)), ''[]'') FROM (' || q || ') t' INTO j;
	RETURN j;
END $$;

-- plan_pushed_aggregates_have_no_case: true when q runs in the scan and no CASE is left in the aggregates it pushes, i.e. agg(CASE WHEN p THEN x ELSE k END) was planned as aggregates
-- with a FILTER (agg/expr/casefilter.c); used by agg_case_filter_else
CREATE FUNCTION sift_helpers.plan_pushed_aggregates_have_no_case(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Pushed Aggregates:' THEN
			IF l ~ 'CASE' THEN RETURN false; END IF;
			pushed := true;
		END IF;
	END LOOP;
	RETURN pushed;
END $$;

-- plan_reads_global_order: true when the plan of q, run with ANALYZE, reads in Global Order
CREATE FUNCTION sift_helpers.plan_reads_global_order(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Global Order: true' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- ids_agree_tt_tp: the ids of q ('@' stands for the table) are the same on tables tt and tp; a difference is a NOTICE with the query
CREATE FUNCTION sift_helpers.ids_agree_tt_tp(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','') FROM (%s) x', replace(q, '@', 'tt')) INTO a;
	EXECUTE format('SELECT string_agg(id::text, '','') FROM (%s) x', replace(q, '@', 'tp')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

-- agg_rows_and_vs_normal_in_session: rows of q with the pushdown and how many differ from the normal plan, in temp tables of the session (no ON COMMIT DROP)
CREATE FUNCTION sift_helpers.agg_rows_and_vs_normal_in_session(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM (TABLE r_on EXCEPT TABLE r_off) x) + (SELECT count(*) FROM (TABLE r_off EXCEPT TABLE r_on) x);
	RESET sift.enable_agg;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- agg_units_kept_share: share of the units a forced pushdown of q kept, as Units / (Units + pruned), rounded to one digit
CREATE FUNCTION sift_helpers.agg_units_kept_share(q text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE l text; kept numeric := 0; pruned numeric := 0;
BEGIN
	PERFORM set_config('sift.agg_force', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ '^ *Units: ' THEN kept := substring(l from '[0-9]+')::numeric; END IF;
		IF l ~ '(\d+) pruned' THEN pruned := substring(l from '(\d+) pruned')::numeric; END IF;
	END LOOP;
	RETURN round(kept / nullif(kept + pruned, 0), 1);
END $$;

-- col_index_matches_heap: ok when the tids sift._test_col_tids returns for idx and preds are the ctids of the table rows that satisfy heapwhere, else the counts
CREATE FUNCTION sift_helpers.col_index_matches_heap(idx regclass, preds text, heapwhere text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE tbl regclass; a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT indrelid::regclass INTO tbl FROM pg_index WHERE indexrelid = idx;
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', tbl, heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L) EXCEPT SELECT ctid FROM %s WHERE %s) z', idx, preds, tbl, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM %s WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L)) z', tbl, heapwhere, idx, preds) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN 'ok' ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

-- plan_label_with_partitions_and_workers: SiftAgg or normal for the plan of q, with the partitions it names and ' with workers' when workers are planned
CREATE FUNCTION sift_helpers.plan_label_with_partitions_and_workers(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; p text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Partitions:%' THEN p := ' ' || trim(split_part(l, ':', 2)); END IF;
		IF l LIKE '%Workers Planned: _%' AND l NOT LIKE '%Workers Planned: 0%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r || p;
END $$;

-- plan_units_and_rows_lines: the Units: and Rows: lines of the EXPLAIN ANALYZE of q, trimmed
CREATE FUNCTION sift_helpers.plan_units_and_rows_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF, ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^ *(Units|Rows):' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;

-- agg_fuse_agrees: q with the pushdown, with sift.agg_fuse off and with the pushdown off return the same rows, and some
CREATE FUNCTION sift_helpers.agg_fuse_agrees(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.agg_fuse = off;
	EXECUTE format('CREATE TEMP TABLE r_alone AS %s', q);
	RESET sift.agg_fuse;
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)
								 UNION ALL (TABLE r_on EXCEPT ALL TABLE r_alone) UNION ALL (TABLE r_alone EXCEPT ALL TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_alone, r_off;
	RETURN d = 0 AND n > 0;
END $$;

-- agg_lookup_cache_differences: rows of q, how many differ without the lookup cache (sift.agg_lookup_cache_kb = 0) and from the normal plan
CREATE FUNCTION sift_helpers.agg_lookup_cache_differences(q text) RETURNS TABLE (result_rows bigint, vs_uncached bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.agg_lookup_cache_kb = 0;
	EXECUTE format('CREATE TEMP TABLE r_gen ON COMMIT DROP AS %s', q);
	RESET sift.agg_lookup_cache_kb;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_uncached := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_gen) UNION ALL (TABLE r_gen EXCEPT TABLE r_on)) d);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_gen, r_off;
	RETURN NEXT;
END $$;

-- agg_pushed_and_differences: whether the plan of q has a SiftAgg scan, its rows, and how many differ from the normal plan (in a transaction)
CREATE FUNCTION sift_helpers.agg_pushed_and_differences(q text) RETURNS TABLE (pushed bool, rows bigint, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  SELECT count(*) INTO rows FROM r_on;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

-- agg_scan_kind_rows_and_differences: SiftAgg or normal by the JSON plan of q, the rows the pushdown returns and how many differ from the normal plan
CREATE FUNCTION sift_helpers.agg_scan_kind_rows_and_differences(q text) RETURNS TABLE (scan text, rows_pushed bigint, differences bigint) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	scan := CASE WHEN jsonb_path_exists(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")') THEN 'SiftAgg' ELSE 'normal' END;
	EXECUTE format('CREATE TEMP TABLE on_rows ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE off_rows ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	rows_pushed := (SELECT count(*) FROM on_rows);
	differences := (SELECT count(*) FROM ((TABLE on_rows EXCEPT TABLE off_rows) UNION ALL (TABLE off_rows EXCEPT TABLE on_rows)) d);
	DROP TABLE on_rows, off_rows;
	RETURN NEXT;
END $$;

-- agg_forced_differences: how many rows of q differ between the normal plan and the pushdown forced on (sift.agg_force), EXCEPT ALL
CREATE FUNCTION sift_helpers.agg_forced_differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- agg_rows_equal_and_not_empty: q returns the same rows with the pushdown and without, and at least one
CREATE FUNCTION sift_helpers.agg_rows_equal_and_not_empty(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND b > 0;
END $$;

-- agg_differences_min_10_rows: how many rows of q differ between the pushdown and the normal plan; an error when the normal plan has fewer than 10 rows
CREATE FUNCTION sift_helpers.agg_differences_min_10_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	IF (SELECT count(*) FROM r_off) < 10 THEN RAISE EXCEPTION 'the series has few rows'; END IF;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- rows_agg_with_workers_and_leader: the rows of q as sorted text with the pushdown on or off, max_parallel_workers_per_gather and parallel_leader_participation set
CREATE FUNCTION sift_helpers.rows_agg_with_workers_and_leader(q text, push bool, workers int, leader bool DEFAULT true) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	PERFORM set_config('parallel_leader_participation', leader::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- rows_agg_with_workers: the rows of q as sorted text with the pushdown on or off and max_parallel_workers_per_gather set
CREATE FUNCTION sift_helpers.rows_agg_with_workers(q text, push bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- agg_plan_has_workers: true when the plan of q with the pushdown on plans workers
CREATE FUNCTION sift_helpers.agg_plan_has_workers(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Workers Planned' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- agg_filters_in_source: true when the EXPLAIN ANALYZE of q with the pushdown on says filters in source
CREATE FUNCTION sift_helpers.agg_filters_in_source(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; s bool := false;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		s := s OR l LIKE '%filters in source%';
	END LOOP;
	RETURN s;
END $$;

-- agg_rows_as_json_equal: the rows of q as json are the same with the pushdown and without
CREATE FUNCTION sift_helpers.agg_rows_as_json_equal(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE n int;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'CREATE TEMP TABLE r_sift ON COMMIT DROP AS SELECT row_to_json(t)::text AS j FROM (' || q || ') t';
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE r_pg ON COMMIT DROP AS SELECT row_to_json(t)::text AS j FROM (' || q || ') t';
	SELECT count(*) INTO n FROM ((TABLE r_sift EXCEPT ALL TABLE r_pg) UNION ALL (TABLE r_pg EXCEPT ALL TABLE r_sift)) d;
	DROP TABLE r_sift, r_pg;
	RETURN n = 0;
END $$;
