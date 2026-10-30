\echo Use "CREATE EXTENSION pg_sift_helpers" to load this file. \quit

-- The SQL helpers the regression tests share (test/sql). SQL only: it needs no test build of the library, so the tests that use it also run on the release library
-- (scripts/release-library-check.sh). Never installed by a normal make install.

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

-- plan_is_agg_scan: the helper that tests of test/sql defined themselves (pushed)
CREATE FUNCTION sift_helpers.plan_is_agg_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false;
BEGIN
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

-- rows_agg_scans: the helper that tests of test/sql defined themselves (au_rows, ca_rows, cp_rows, kf_rows, kr_rows, sp_rows)
CREATE FUNCTION sift_helpers.rows_agg_scans(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- plan_has_agg_scan: the helper that tests of test/sql defined themselves (a_pushed, d_pushed, j_pushed, k_pushed, t_pushed)
CREATE FUNCTION sift_helpers.plan_has_agg_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
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

-- agg_mode_workers_all_scans: the helper that 5 tests of test/sql defined themselves (oc_mode, ta_mode, tb_mode, tc_mode)
CREATE FUNCTION sift_helpers.agg_mode_workers_all_scans(pushed bool, workers int) RETURNS void LANGUAGE plpgsql AS $$
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

-- rows_agg_all_scans: the helper that 6 tests of test/sql defined themselves (au_rows, ca_rows, cp_rows, kf_rows, kr_rows, sp_rows)
CREATE FUNCTION sift_helpers.rows_agg_all_scans(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- rows_agg_all_scans_errors: the helper that 4 tests of test/sql defined themselves (kp_rows, kt_rows, ss_rows, tk_rows)
CREATE FUNCTION sift_helpers.rows_agg_all_scans_errors(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
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
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;

-- plan_mentions_siftagg: the helper that 4 tests of test/sql defined themselves (chars_pushed, im_pushed, long_pushed, wd_pushed)
CREATE FUNCTION sift_helpers.plan_mentions_siftagg(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
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
