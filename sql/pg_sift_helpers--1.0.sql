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
