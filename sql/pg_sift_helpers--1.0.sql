\echo Use "CREATE EXTENSION pg_sift_helpers" to load this file. \quit

-- The SQL helpers the regression tests share (test/sql). SQL only: it needs no test build of the library, so the tests that use it also run on the release library
-- (scripts/release-library-check.sh). Never installed by a normal make install. A plan "has the pushdown" when a line names a SiftAgg* custom scan (SiftAgg, SiftAggV6, SiftAggV6Sip...).

-- The tests that run as a role without rights (row security, grants) call the helpers too
GRANT USAGE ON SCHEMA sift_helpers TO PUBLIC;

-- margin_note: with sift_helpers.margin = on, the price of the scan and of the normal plan of each arm of q (scripts/plan-margin.sh --tests); plan_text calls it
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

-- plan_text: the EXPLAIN of q as one text, its lines in plan order (run: EXPLAIN ANALYZE without timings, summary and buffers)
CREATE FUNCTION sift_helpers.plan_text(q text, run bool DEFAULT false) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	PERFORM sift_helpers.margin_note(q);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF' || CASE WHEN run THEN ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF' ELSE '' END || ') ' || q LOOP r := r || l || E'\n'; END LOOP;
	RETURN rtrim(r, E'\n');
END $$;

-- plan_last: the first match of the regular expression rx (its capture, else the whole match) in the last line of the plan that has one, NULL without a match
CREATE FUNCTION sift_helpers.plan_last(plan text, rx text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT substring(l from rx) FROM regexp_split_to_table(plan, E'\n') WITH ORDINALITY t(l, n) WHERE l ~ rx ORDER BY n DESC LIMIT 1 $$;

-- plan_pushed: the plan has a SiftAgg* node
CREATE FUNCTION sift_helpers.plan_pushed(plan text) RETURNS bool LANGUAGE sql IMMUTABLE AS $$ SELECT plan LIKE '%Custom Scan (SiftAgg%' $$;

-- plan_label: SiftAgg or normal for the plan, with the partitions it names and ' with workers' for each line that plans workers
CREATE FUNCTION sift_helpers.plan_label(q text, partitions bool DEFAULT false) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; p text := '';
BEGIN
	FOR l IN SELECT regexp_split_to_table(sift_helpers.plan_text(q), E'\n') LOOP
		IF sift_helpers.plan_pushed(l) THEN r := 'SiftAgg'; END IF;
		IF partitions AND l LIKE '%Partitions:%' THEN p := ' ' || trim(split_part(l, ':', 2)); END IF;
		IF l LIKE '%Workers Planned: _%' AND l NOT LIKE '%Workers Planned: 0%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r || p;
END $$;

-- set_scans: the table scans on (the normal plan) or off (the pushdown), the index-only scan too unless ios is false
CREATE FUNCTION sift_helpers.set_scans(enabled bool, ios bool DEFAULT true) RETURNS void LANGUAGE sql AS $$
	SELECT set_config(s, enabled::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan'] || CASE WHEN ios THEN ARRAY['enable_indexonlyscan'] ELSE '{}' END) s $$;

-- set_arm: sift.enable_agg for one arm of a comparison, and sift.agg_force when forced
CREATE FUNCTION sift_helpers.set_arm(pushed bool, force bool DEFAULT false) RETURNS void LANGUAGE sql AS $$
	SELECT set_config('sift.enable_agg', pushed::text, true), CASE WHEN force THEN set_config('sift.agg_force', 'on', true) END $$;

-- sorted_rows: the rows of q as text, sorted and joined by sep; empty for no rows (NULL by default)
CREATE FUNCTION sift_helpers.sorted_rows(q text, empty text DEFAULT NULL, sep text DEFAULT E'\n') RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(x::text, %L ORDER BY x::text), %L) FROM (%s) x', sep, empty, q) INTO r;
	RETURN r;
END $$;

-- table_diff: the rows in only one of two tables, EXCEPT ALL or EXCEPT
CREATE FUNCTION sift_helpers.table_diff(a text, b text, except_all bool DEFAULT true) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((TABLE %1$s EXCEPT%3$s TABLE %2$s) UNION ALL (TABLE %2$s EXCEPT%3$s TABLE %1$s)) d', a, b, CASE WHEN except_all THEN ' ALL' ELSE '' END) INTO d;
	RETURN d;
END $$;

-- agg_check: how many rows of q differ between the normal plan and the pushdown (-1 for no rows with require_nonempty); force: sift.agg_force on, scans_off: table scans off
CREATE FUNCTION sift_helpers.agg_check(q text, force bool DEFAULT false, except_all bool DEFAULT true, require_nonempty bool DEFAULT false, scans_off bool DEFAULT false)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	PERFORM sift_helpers.set_arm(false);
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	PERFORM sift_helpers.set_arm(true, force);
	IF scans_off THEN PERFORM sift_helpers.set_scans(false); END IF;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	d := sift_helpers.table_diff('r_on', 'r_off', except_all);
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN CASE WHEN require_nonempty AND n = 0 THEN -1 ELSE d END;
END $$;

-- plan_has_agg: the plan of q with the pushdown on (forced, table scans off) has a SiftAgg* node
CREATE FUNCTION sift_helpers.plan_has_agg(q text, force bool DEFAULT false, scans_off bool DEFAULT false) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(true, force);
	IF scans_off THEN PERFORM sift_helpers.set_scans(false); END IF;
	RETURN sift_helpers.plan_pushed(sift_helpers.plan_text(q));
END $$;

-- f2_same: 'same' when q returns the rows through a bitmap scan of the index and through the heap (index_only false: index-only scans off; detail: say both answers when they differ)
CREATE FUNCTION sift_helpers.f2_same(q text, detail bool DEFAULT true, index_only bool DEFAULT true) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	PERFORM set_config(s, 'off', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan'] || CASE WHEN index_only THEN '{}' ELSE ARRAY['enable_indexonlyscan'] END) s;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	via_index := sift_helpers.sorted_rows(q, '', ',');
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	via_heap := sift_helpers.sorted_rows(q, '', ',');
	RETURN CASE WHEN via_index = via_heap THEN 'same' WHEN detail THEN 'DIFFERENT: ' || via_index || ' / ' || via_heap ELSE 'DIFFERENT' END;
END $$;

-- same_rows: the rows of the queries a and b are the same
CREATE FUNCTION sift_helpers.same_rows(a text, b text) RETURNS bool LANGUAGE sql AS $$
	SELECT sift_helpers.sorted_rows(a, '') = sift_helpers.sorted_rows(b, '') $$;

-- rows_md5: the md5 of the sorted rows of q (a SELECT or an EXECUTE), of the empty string for none
CREATE FUNCTION sift_helpers.rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS scope_rows;
	EXECUTE 'CREATE TEMP TABLE scope_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM scope_rows t;
	DROP TABLE scope_rows;
	RETURN r;
END $$;

-- assert_eq: 'name: ok', an error with both values when got differs from want
CREATE FUNCTION sift_helpers.assert_eq(name text, got anyelement, want anyelement) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF got IS DISTINCT FROM want THEN RAISE EXCEPTION 'FAILED %: got %, want %', name, got, want; END IF;
	RETURN name || ': ok';
END $$;

-- statement_outcome: 'ok' when the statement runs, else its SQLSTATE and message joined by sep
CREATE FUNCTION sift_helpers.statement_outcome(stmt text, sep text DEFAULT ' ') RETURNS text LANGUAGE plpgsql AS $$
BEGIN EXECUTE stmt; RETURN 'ok'; EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE || sep || SQLERRM; END $$;

-- try_sql: 'ok' when the statement runs, else its message with the hint
CREATE FUNCTION sift_helpers.try_sql(stmt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE hint text;
BEGIN EXECUTE stmt; RETURN 'ok'; EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN SQLERRM || coalesce(' [' || hint || ']', ''); END $$;

-- answer: the rows of q as sorted text joined by ';' with the pushdown on or off
CREATE FUNCTION sift_helpers.answer(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(scan);
	RETURN sift_helpers.sorted_rows(q, NULL, ';');
END $$;

-- rows_agg and its variants: the rows of q as sorted text with the pushdown on or off (four_scans: the table scans the opposite, no rows is 'no rows')
CREATE FUNCTION sift_helpers.rows_agg(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN PERFORM sift_helpers.set_arm(push); RETURN sift_helpers.sorted_rows(q); END $$;
CREATE FUNCTION sift_helpers.rows_agg_four_scans_or_no_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN PERFORM sift_helpers.set_arm(push); PERFORM sift_helpers.set_scans(NOT push); RETURN sift_helpers.sorted_rows(q, 'no rows'); END $$;
CREATE FUNCTION sift_helpers.rows_agg_other_scans_off(q text, push bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_four_scans_or_no_rows(q, push) $$;
CREATE FUNCTION sift_helpers.rows_agg_four_scans_error_text(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN RETURN sift_helpers.rows_agg_four_scans_or_no_rows(q, push); EXCEPTION WHEN OTHERS THEN RETURN 'ERROR: ' || SQLERRM; END $$;
CREATE FUNCTION sift_helpers.rows_agg_preagg_and_pushdown(q text, pre bool, push bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN RETURN sift_helpers.rows_agg(q, push); END $$;
CREATE FUNCTION sift_helpers.rows_agg_with_workers(q text, push bool, workers int, leader bool DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	IF leader IS NOT NULL THEN PERFORM set_config('parallel_leader_participation', leader::text, true); END IF;
	RETURN sift_helpers.rows_agg(q, push);
END $$;
CREATE FUNCTION sift_helpers.rows_agg_with_workers_and_leader(q text, push bool, workers int, leader bool DEFAULT true) RETURNS text LANGUAGE sql AS $$
	SELECT sift_helpers.rows_agg_with_workers(q, push, workers, leader) $$;
CREATE FUNCTION sift_helpers.rows_agg_with_workers_reset(q text, scan bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM sift_helpers.set_arm(scan);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	r := sift_helpers.sorted_rows(q, '');
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN r;
END $$;
CREATE FUNCTION sift_helpers.rows_agg_as_jsonb_array(q text, push bool) RETURNS jsonb[] LANGUAGE plpgsql AS $$
DECLARE r jsonb[];
BEGIN
	PERFORM sift_helpers.set_arm(push);
	EXECUTE format('SELECT array_agg(to_jsonb(q) ORDER BY q.k) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION sift_helpers.rows_as_jsonb(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t)), ''[]'') FROM (' || q || ') t' INTO j; RETURN j; END $$;
CREATE FUNCTION sift_helpers.count_agg_pushdown_set(q text, scan bool) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN PERFORM sift_helpers.set_arm(scan); EXECUTE q INTO n; RETURN n; END $$;

-- agg_mode and its variants: the settings of one arm (the table scans off for the pushdown, on for the normal plan)
CREATE FUNCTION sift_helpers.agg_mode(pushed bool) RETURNS void LANGUAGE sql AS $$
	SELECT sift_helpers.set_arm(pushed), sift_helpers.set_scans(NOT pushed, false), set_config('max_parallel_workers_per_gather', '0', true) $$;
CREATE FUNCTION sift_helpers.agg_mode_workers(pushed bool, workers int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.agg_mode(pushed);
	PERFORM set_config('max_parallel_workers_per_gather', CASE WHEN pushed THEN workers ELSE 0 END::text, true);
	PERFORM set_config(s, '0', true) FROM unnest(ARRAY['parallel_setup_cost', 'parallel_tuple_cost', 'min_parallel_table_scan_size']) s;
END $$;
CREATE FUNCTION sift_helpers.agg_mode_scan_and_count_off(pushed bool) RETURNS void LANGUAGE sql AS $$
	SELECT sift_helpers.set_arm(pushed), sift_helpers.set_scans(NOT pushed, false), set_config('sift.enable_scan', (NOT pushed)::text, true) $$;

-- the comparison of the pushdown with the normal plan, as the tests ask for it
CREATE FUNCTION sift_helpers.differences_agg(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.agg_check(q) $$;
CREATE FUNCTION sift_helpers.differences_agg_temp(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.agg_check(q) $$;
CREATE FUNCTION sift_helpers.agg_forced_differences(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.agg_check(q, force => true) $$;
CREATE FUNCTION sift_helpers.agg_equals_normal(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_check(q, except_all => false) = 0 $$;
CREATE FUNCTION sift_helpers.agg_equals_normal_nonempty(q text) RETURNS bool LANGUAGE sql AS $$
	SELECT sift_helpers.agg_check(q, except_all => false, require_nonempty => true) = 0 $$;
CREATE FUNCTION sift_helpers.agg_rows_equal_and_not_empty(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal_nonempty(q) $$;
CREATE FUNCTION sift_helpers.agg_equals_normal_all(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_check(q, require_nonempty => true) = 0 $$;
CREATE FUNCTION sift_helpers.agg_differences_min_10_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM sift_helpers.set_arm(false);
	EXECUTE format('SELECT count(*) FROM (%s) x', q) INTO n;
	IF n < 10 THEN RAISE EXCEPTION 'the series has few rows'; END IF;
	RETURN sift_helpers.agg_check(q, except_all => false);
END $$;

-- agg_check_outcome: 'scan same', 'plain same' or '... DIFF n' by the plan of q and the rows of both arms, 'error: ...' or 'DIFF on=... off=...' when an arm fails
CREATE FUNCTION sift_helpers.agg_check_outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; pushed boolean; e_on text; e_off text;
BEGIN
	SET sift.enable_agg = on;
	pushed := sift_helpers.plan_pushed(sift_helpers.plan_text(q));
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
	SELECT count(*) INTO a FROM ((SELECT d::text FROM r_on d EXCEPT ALL SELECT d::text FROM r_off d) UNION ALL (SELECT d::text FROM r_off d EXCEPT ALL SELECT d::text FROM r_on d)) x;
	DROP TABLE r_on, r_off;
	RETURN (CASE WHEN pushed THEN 'scan ' ELSE 'plain ' END) || CASE WHEN a = 0 THEN 'same' ELSE 'DIFF ' || a END;
END $$;

-- the plans of q: whether and how the pushdown shows
CREATE FUNCTION sift_helpers.plan_has_agg_scan(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_has_agg(q) $$;
CREATE FUNCTION sift_helpers.plan_mentions_siftagg(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_has_agg(q) $$;
CREATE FUNCTION sift_helpers.plan_has_siftagg_pushdown_on(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_has_agg(q) $$;
CREATE FUNCTION sift_helpers.plan_has_siftagg_with_scans_off(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_has_agg(q, scans_off => true) $$;
CREATE FUNCTION sift_helpers.plan_mentions_siftagg_forced(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_has_agg(q, force => true) $$;
CREATE FUNCTION sift_helpers.plan_mentions_siftagg_regex(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_pushed(sift_helpers.plan_text(q)) $$;
CREATE FUNCTION sift_helpers.plan_siftagg_or_normal(q text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN sift_helpers.plan_pushed(sift_helpers.plan_text(q)) THEN 'SiftAgg' ELSE 'normal' END $$;
CREATE FUNCTION sift_helpers.plan_is_agg_scan(q text) RETURNS bool LANGUAGE sql AS $$
	SELECT sift_helpers.plan_pushed(p) AND p !~ '(SiftAggRows|GroupAggregate|HashAggregate)' FROM (SELECT sift_helpers.plan_text(q) p) x $$;
CREATE FUNCTION sift_helpers.plan_label_with_partitions_and_workers(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_label(q, true) $$;
CREATE FUNCTION sift_helpers.plan_siftagg_with_workers_label(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_label(q) $$;
CREATE FUNCTION sift_helpers.plan_mode_workers(q text, workers int, planned text DEFAULT '') RETURNS text LANGUAGE plpgsql AS $$
DECLARE p text;
BEGIN
	PERFORM sift_helpers.set_arm(true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	p := sift_helpers.plan_text(q);
	RETURN CASE WHEN sift_helpers.plan_pushed(p) AND p LIKE '%Workers Planned: ' || planned || '%' THEN 'parallel' WHEN sift_helpers.plan_pushed(p) THEN 'serial' ELSE 'none' END;
END $$;
CREATE FUNCTION sift_helpers.plan_mode_two_workers(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_mode_workers(q, 2, '2') $$;
CREATE FUNCTION sift_helpers.agg_plan_has_workers(q text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN PERFORM sift_helpers.set_arm(true); RETURN sift_helpers.plan_text(q) LIKE '%Workers Planned%'; END $$;
CREATE FUNCTION sift_helpers.plan_siftagg_nodes(q text) RETURNS int LANGUAGE sql AS $$
	SELECT count(*)::int FROM regexp_matches(sift_helpers.plan_text(q), 'Custom Scan \(SiftAgg', 'g') $$;
CREATE FUNCTION sift_helpers.plan_siftagg_nodes_cached_tables(q text) RETURNS text LANGUAGE sql AS $$
	SELECT format('%s SiftAgg nodes, %s cached tables', (SELECT count(*) FROM regexp_matches(p, 'Custom Scan \(SiftAgg', 'g')),
		(SELECT coalesce(sum(m[1]::int), 0) FROM regexp_matches(p, '(\d+) cached tables', 'g') m)) FROM (SELECT sift_helpers.plan_text(q, true) p) x $$;
CREATE FUNCTION sift_helpers.plan_limit_hint(q text) RETURNS int LANGUAGE sql AS $$
	SELECT coalesce(substring(sift_helpers.plan_text(q) from 'Limit Hint: (\d+)')::int, 0) $$;
CREATE FUNCTION sift_helpers.plan_reads_global_order(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_text(q, true) LIKE '%Global Order: true%' $$;
CREATE FUNCTION sift_helpers.plan_pushed_aggregates_have_no_case(q text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(true);
	RETURN coalesce((SELECT bool_and(l !~ 'CASE') FROM regexp_split_to_table(sift_helpers.plan_text(q), E'\n') l WHERE l ~ 'Pushed Aggregates:'), false);
END $$;
CREATE FUNCTION sift_helpers.plan_group_keys_text(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(true);
	RETURN coalesce((SELECT string_agg(trim(l), '' ORDER BY n) FROM regexp_split_to_table(sift_helpers.plan_text(q), E'\n') WITH ORDINALITY t(l, n) WHERE l LIKE '%Group Keys:%'), '');
END $$;
CREATE FUNCTION sift_helpers.plan_units_lines(q text) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(string_agg(' ' || trim(l), '' ORDER BY n), '') FROM regexp_split_to_table(sift_helpers.plan_text(q, true), E'\n') WITH ORDINALITY t(l, n) WHERE l LIKE '%Units: %' $$;
CREATE FUNCTION sift_helpers.plan_units_and_rows_lines(q text) RETURNS SETOF text LANGUAGE sql AS $$
	SELECT trim(l) FROM regexp_split_to_table(sift_helpers.plan_text(q, true), E'\n') WITH ORDINALITY t(l, n) WHERE l ~ '^ *(Units|Rows):' ORDER BY n $$;
CREATE FUNCTION sift_helpers.plan_heap_rereads_forced_on(q text) RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(true);
	RETURN coalesce(sift_helpers.plan_last(sift_helpers.plan_text(q, true), 'Rows Read Again From The Heap: (\d+)')::bigint, 0);
END $$;
CREATE FUNCTION sift_helpers.agg_heap_rereads(q text) RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN RETURN sift_helpers.plan_heap_rereads_forced_on(q); EXCEPTION WHEN others THEN RETURN -1; END $$;
CREATE FUNCTION sift_helpers.agg_arms_in_one_pass(q text) RETURNS int LANGUAGE sql AS $$
	SELECT coalesce(max(m[1]::int), 0) FROM regexp_matches(sift_helpers.plan_text(q, true), 'Fused Arms: (\d+)', 'g') m $$;
-- rows_decoded: the rows the sift scans of this backend decoded to run q (sift.stats rows_decoded)
CREATE FUNCTION sift_helpers.rows_decoded(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE r0 bigint := (SELECT value FROM sift.stats(local => true) WHERE name = 'rows_decoded');
BEGIN
	EXECUTE q;
	RETURN (SELECT value FROM sift.stats(local => true) WHERE name = 'rows_decoded') - r0;
END $$;
-- agg_units_read: 'normal', or the granules the SiftAggV6 nodes of the plan read (scanned or answered from the pre-aggregates)
CREATE FUNCTION sift_helpers.agg_units_read(q text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN NOT sift_helpers.plan_pushed(p) THEN 'normal'
		ELSE format('SiftAgg read %s', coalesce((SELECT sum(m[1]::int) FROM regexp_matches(p, '(?:scanned|preagg)=(\d+)', 'g') m), 0)) END
	FROM (SELECT sift_helpers.plan_text(q, true) p) x $$;
CREATE FUNCTION sift_helpers.agg_units_kept_share(q text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE p text; line text; kept numeric; pruned numeric;
BEGIN
	PERFORM set_config('sift.agg_force', 'on', true);
	p := sift_helpers.plan_text(q, true);
	line := sift_helpers.plan_last(p, '^ *(Granules: scanned=.*)$');
	kept := coalesce(substring(line from 'scanned=(\d+)')::numeric, 0);
	pruned := coalesce((SELECT sum(m[1]::numeric) FROM regexp_matches(line, ' pruned_\w+=(\d+)', 'g') m), 0);
	RETURN round(kept / nullif(kept + pruned, 0), 1);
END $$;
CREATE FUNCTION sift_helpers.units_read_scans_off(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE p text;
BEGIN
	PERFORM sift_helpers.set_arm(true);
	PERFORM sift_helpers.set_scans(false);
	p := sift_helpers.plan_text(q, true);
	RETURN CASE WHEN sift_helpers.plan_pushed(p) THEN coalesce(sift_helpers.plan_last(p, '^ *Granules: scanned=(\d+)')::int, 0) END;
END $$;
CREATE FUNCTION sift_helpers.agg_filters_in_source(q text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN PERFORM sift_helpers.set_arm(true); RETURN sift_helpers.plan_text(q, true) LIKE '%filters in source%'; END $$;
CREATE FUNCTION sift_helpers.agg_index_used(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(true);
	RETURN coalesce((SELECT btrim(l) FROM regexp_split_to_table(sift_helpers.plan_text(q), E'\n') WITH ORDINALITY t(l, n) WHERE l ~ '^\s*Index: ' ORDER BY n LIMIT 1), 'native');
END $$;
CREATE FUNCTION sift_helpers.plan_siftagg_with_lookup_counts(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN SELECT regexp_split_to_table(sift_helpers.plan_text(q, true), E'\n') LOOP
		IF sift_helpers.plan_pushed(l) THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Lookup: %' THEN r := r || ',' || concat_ws(',', substring(l from '\d+ rows read'), substring(l from '\d+ rows loaded'), substring(l from '\d+ index scans')); END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION sift_helpers.plan_siftagg_with_text_key_filters(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN SELECT regexp_split_to_table(sift_helpers.plan_text(q, true), E'\n') LOOP
		IF sift_helpers.plan_pushed(l) THEN r := 'SiftAgg'; END IF;
		IF l ~ '(\d+) text key filters' THEN r := r || ' text key filters ' || substring(l from '(\d+) text key filters'); END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION sift_helpers.plan_engaged_and_answers_same(q text) RETURNS TABLE (engaged bool, same bool) LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	engaged := sift_helpers.plan_pushed(sift_helpers.plan_text(q));
	PERFORM sift_helpers.set_arm(true);
	a := md5(sift_helpers.sorted_rows(q, NULL, '|'));
	PERFORM sift_helpers.set_arm(false);
	b := md5(sift_helpers.sorted_rows(q, NULL, '|'));
	PERFORM sift_helpers.set_arm(true);
	same := a IS NOT DISTINCT FROM b;
	RETURN NEXT;
END $$;

-- the rows of q with and without the pushdown, and how many differ (session_tables: temp tables that live in the session, not to the commit)
CREATE FUNCTION sift_helpers.agg_rows_diff(q text, session_tables bool DEFAULT false) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE keep text := CASE WHEN session_tables THEN '' ELSE 'ON COMMIT DROP' END;
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on %s AS %s', keep, q);
	PERFORM sift_helpers.set_arm(false);
	EXECUTE format('CREATE TEMP TABLE r_off %s AS %s', keep, q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := sift_helpers.table_diff('r_on', 'r_off', false);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;
CREATE FUNCTION sift_helpers.agg_rows_and_differences(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_rows_diff(q) $$;
CREATE FUNCTION sift_helpers.agg_rows_and_vs_normal_in_session(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$
	SELECT * FROM sift_helpers.agg_rows_diff(q, true) $$;
CREATE FUNCTION sift_helpers.agg_plan_and_differences(plan_label text, q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$
	SELECT plan_label, d.* FROM sift_helpers.agg_rows_diff(q) d $$;
CREATE FUNCTION sift_helpers.agg_scan_kind_rows_and_differences(q text) RETURNS TABLE (scan text, rows_pushed bigint, differences bigint) LANGUAGE plpgsql AS $$
BEGIN
	scan := CASE WHEN sift_helpers.plan_pushed(sift_helpers.plan_text(q, true)) THEN 'SiftAgg' ELSE 'normal' END;
	SELECT d.result_rows, d.vs_normal_plan INTO rows_pushed, differences FROM sift_helpers.agg_rows_diff(q) d;
	RETURN NEXT;
END $$;
CREATE FUNCTION sift_helpers.agg_pushed_and_differences(q text) RETURNS TABLE (pushed bool, rows bigint, differences bigint) LANGUAGE plpgsql AS $$
BEGIN
	differences := sift_helpers.agg_check(q);
	EXECUTE format('SELECT count(*) FROM (%s) x', q) INTO rows;
	pushed := sift_helpers.plan_pushed(sift_helpers.plan_text(q));
	RETURN NEXT;
END $$;
CREATE FUNCTION sift_helpers.agg_lookup_cache_differences(q text) RETURNS TABLE (result_rows bigint, vs_uncached bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	PERFORM set_config('sift.backend_cache_kb', '0', true);
	EXECUTE format('CREATE TEMP TABLE r_gen ON COMMIT DROP AS %s', q);
	RESET sift.backend_cache_kb;
	PERFORM sift_helpers.set_arm(false);
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_uncached := sift_helpers.table_diff('r_on', 'r_gen', false);
	vs_normal_plan := sift_helpers.table_diff('r_on', 'r_off', false);
	DROP TABLE r_on, r_gen, r_off;
	RETURN NEXT;
END $$;
CREATE FUNCTION sift_helpers.agg_fuse_agrees(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	PERFORM sift_helpers.set_arm(true);
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_alone AS %s', q);
	PERFORM sift_helpers.set_arm(false);
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	d := sift_helpers.table_diff('r_on', 'r_off') + sift_helpers.table_diff('r_on', 'r_alone');
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_alone, r_off;
	RETURN d = 0 AND n > 0;
END $$;
CREATE FUNCTION sift_helpers.agg_rows_as_json_equal(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	PERFORM sift_helpers.set_arm(true);
	EXECUTE 'CREATE TEMP TABLE r_sift ON COMMIT DROP AS SELECT row_to_json(t)::text AS j FROM (' || q || ') t';
	PERFORM sift_helpers.set_arm(false);
	EXECUTE 'CREATE TEMP TABLE r_pg ON COMMIT DROP AS SELECT row_to_json(t)::text AS j FROM (' || q || ') t';
	d := sift_helpers.table_diff('r_sift', 'r_pg');
	DROP TABLE r_sift, r_pg;
	RETURN d = 0;
END $$;
CREATE FUNCTION sift_helpers.groups_differences_sift_vs_heap(t regclass) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	PERFORM sift_helpers.set_arm(true);
	PERFORM set_config('seq_page_cost', '10000', true);
	EXECUTE format('CREATE TEMP TABLE by_sift ON COMMIT DROP AS SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g', t);
	PERFORM sift_helpers.set_arm(false);
	RESET seq_page_cost;
	EXECUTE format('CREATE TEMP TABLE by_heap ON COMMIT DROP AS SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g', t);
	d := sift_helpers.table_diff('by_sift', 'by_heap', false);
	DROP TABLE by_sift, by_heap;
	RETURN d;
END $$;

-- topn_arm: the settings of the forced pushdown that reads in Global Order, or of the normal plan after it
CREATE FUNCTION sift_helpers.topn_arm(forced bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift_helpers.set_arm(forced, forced);
	PERFORM set_config('enable_sort', (NOT forced)::text, true);
	IF forced THEN
		PERFORM sift_helpers.set_scans(false);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
	ELSE
		PERFORM set_config('enable_seqscan', 'on', true);
	END IF;
END $$;
-- topn_check: DIFFERS plus q when the ordered rows (expr over x, the numbered rows) differ between the forced pushdown and the normal plan, else units without Global Order,
-- global order, or with a limit on the heap fetches skipped or fetched
CREATE FUNCTION sift_helpers.topn_check(q text, expr text DEFAULT 'x::text', max_fetches int DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
DECLARE p text; ra text; rb text;
	rows_q text := format('SELECT string_agg(%s, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', expr, q);
BEGIN
	PERFORM sift_helpers.topn_arm(true);
	p := sift_helpers.plan_text(q, true);
	EXECUTE rows_q INTO ra;
	PERFORM sift_helpers.topn_arm(false);
	EXECUTE rows_q INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN p NOT LIKE '%Global Order%' THEN 'units' WHEN max_fetches IS NULL THEN 'global order'
		WHEN coalesce(sift_helpers.plan_last(p, '([0-9]+) heap fetches')::int, -1) <= max_fetches THEN 'skipped' ELSE 'fetched' END;
END $$;
CREATE FUNCTION sift_helpers.topn_check_ordered_rows_with_nulls(q text) RETURNS text LANGUAGE sql AS $$
	SELECT sift_helpers.topn_check(q, 'coalesce(x::text, ''null'')') $$;
CREATE FUNCTION sift_helpers.topn_check_ordered_rows(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.topn_check(q) $$;
CREATE FUNCTION sift_helpers.topn_offset_check_heap_fetches(q text, lim int, off int) RETURNS text LANGUAGE sql AS $$
	SELECT sift_helpers.topn_check(q, $e$id || ':' || n || ':' || s$e$, lim + off / 8) $$;
CREATE FUNCTION sift_helpers.count_check_rows_scanned_under_5000(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE p text; ra text; rb text;
BEGIN
	PERFORM sift_helpers.topn_arm(true);
	p := sift_helpers.plan_text(q, true);
	EXECUTE q INTO ra;
	PERFORM sift_helpers.topn_arm(false);
	EXECUTE q INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q
		WHEN EXISTS (SELECT 1 FROM regexp_matches(p, 'Rows: ([0-9]+) scanned', 'g') m WHERE m[1]::int < 5000) THEN 'counted' ELSE 'units' END;
END $$;
CREATE FUNCTION sift_helpers.topn_offset_rows_twice_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
	rows_q text := format('SELECT string_agg(g || '':'' || id, '','' ORDER BY g, ord) FROM generate_series(1, 3) g, (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', q);
BEGIN
	PERFORM sift_helpers.set_arm(true, true);
	PERFORM set_config(s, 'off', true) FROM unnest(ARRAY['enable_seqscan', 'enable_sort', 'enable_material', 'enable_hashjoin', 'enable_mergejoin']) s;
	PERFORM set_config('sift.enable_col_scan', 'on', true);
	EXECUTE rows_q INTO a;
	PERFORM sift_helpers.topn_arm(false);
	EXECUTE rows_q INTO b;
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE 'DIFFERS ' || q END;
END $$;

-- the tables of one test each
CREATE FUNCTION sift_helpers.events_sums_with_plan_check(want_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text := 'SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id'; p text := sift_helpers.plan_text(q); r text;
BEGIN
	IF sift_helpers.plan_pushed(p) <> want_sift THEN RAISE EXCEPTION 'the plan is % a SiftAgg plan: %', CASE WHEN want_sift THEN 'not' ELSE 'still' END, p || E'\n'; END IF;
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', tenant_id, n, s), '' '' ORDER BY tenant_id), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;
-- the heap blocks the execution of the aggregate fetched beyond the one every read of a vacuumed table takes: those of the whole statement less those of planning it
-- (a plan with sift costing samples the heap; the second EXPLAIN is the planning alone)
CREATE FUNCTION sift_helpers.events_heap_pages() RETURNS int LANGUAGE plpgsql AS $$
DECLARE q constant text := 'SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY tenant_id'; b0 bigint; b1 bigint; b2 bigint;
BEGIN
	EXECUTE 'EXPLAIN ' || q;
	b0 := coalesce(pg_stat_get_xact_blocks_fetched('events'::regclass), 0);
	EXECUTE 'EXPLAIN ' || q;
	b1 := coalesce(pg_stat_get_xact_blocks_fetched('events'::regclass), 0);
	EXECUTE q;
	b2 := coalesce(pg_stat_get_xact_blocks_fetched('events'::regclass), 0);
	RETURN b2 - b1 - (b1 - b0) - 1;
END $$;
CREATE FUNCTION sift_helpers.tk_sums_with_plan_check(mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE p text;
BEGIN
	PERFORM sift_helpers.set_arm(mode < 2);
	PERFORM set_config('sift.test_no_heapfree', (mode = 1)::text, true);
	p := sift_helpers.plan_text('SELECT tenant, count(*), sum(amount) FROM tk GROUP BY tenant');
	IF sift_helpers.plan_pushed(p) <> (mode < 2) THEN RAISE EXCEPTION 'the plan is not the one asked for: %', p || E'\n'; END IF;
	RETURN coalesce((SELECT string_agg(format('%s:%s:%s', tenant, n, s), ' ' ORDER BY tenant) FROM (SELECT tenant, count(*) AS n, sum(amount) AS s FROM tk GROUP BY tenant) q), 'none');
END $$;
-- the heap blocks the grouped aggregate of t fetched, less the pages of the visibility map a read looks at once (a view has none), -1 when the plan is not the aggregate scan
CREATE FUNCTION sift_helpers.grouped_read_heap_pages(t regclass) RETURNS bigint LANGUAGE plpgsql SET sift.enable_agg = on SET seq_page_cost = 10000 AS $$
DECLARE q constant text := format('SELECT g, count(*), sum(v) FROM %s GROUP BY g', t); b0 bigint; b1 bigint; b2 bigint;
BEGIN
	IF NOT sift_helpers.plan_pushed(sift_helpers.plan_text(q)) THEN RETURN -1; END IF;
	b0 := coalesce(pg_stat_get_xact_blocks_fetched(t), 0);
	EXECUTE 'EXPLAIN ' || q;
	b1 := coalesce(pg_stat_get_xact_blocks_fetched(t), 0);
	EXECUTE q;
	b2 := coalesce(pg_stat_get_xact_blocks_fetched(t), 0);
	RETURN b2 - b1 - (b1 - b0) - pg_relation_size(t, 'vm') / 8192;
END $$;
CREATE FUNCTION sift_helpers.rows_agree(q text, col text, a_rel text, b_rel text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(%I::text, '','') FROM (%s) x', col, replace(q, '@', a_rel)) INTO a;
	EXECUTE format('SELECT string_agg(%I::text, '','') FROM (%s) x', col, replace(q, '@', b_rel)) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
CREATE FUNCTION sift_helpers.ids_agree_tt_tp(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.rows_agree(q, 'id', 'tt', 'tp') $$;
CREATE FUNCTION sift_helpers.same_keys_in_t_and_p(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.rows_agree(q, 'k', 't', 'p') $$;
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
CREATE FUNCTION sift_helpers.agg_events_parts_arms_query() RETURNS text LANGUAGE sql AS $$
	SELECT $q$(SELECT e.id, e.parent_id, e.kind, e.ts, o.cat AS category, avg(o.score) AS score, count(*) AS n
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts, o.cat
	           UNION ALL
	           SELECT e.id, e.parent_id, e.kind, e.ts, NULL::text, avg(o.score), count(*)
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts) m$q$
$$;
CREATE FUNCTION sift_helpers.index_key_cut_reloption(i regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = i AND o ~ '^key_cut'), 'none') $$;
CREATE FUNCTION sift_helpers.words_from_seed_list(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg((ARRAY['fast','late','damaged','refund','gift','invoice','exchange','missing','perfect','broken','express','pickup',
	                         'thanks','wrong','size','color','cable','wire','box','label'])[1 + (hashint4(seed * 31 + w) & 2147483647) % 20], ' ')
	FROM generate_series(1, 3 + seed % 5) w $$;
CREATE FUNCTION sift_helpers.collation_for_text_order() RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT collname FROM pg_collation WHERE collname IN ('en_US.utf8', 'und-x-icu') ORDER BY collname LIMIT 1), 'C') $$;
