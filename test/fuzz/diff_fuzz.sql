-- Runner of the differential fuzz (test/fuzz/diff_fuzz.py). fz.ref runs a query under the settings of the reference, fz.var under those of a
-- variant, and compares with coldiff.compare (test/col/run.sql, the contract of docs/AGG.md "Differential suite": exact except floats,
-- which are equal within 1e-12 + 1e-9 relative; rows by position when keycols is NULL, else sorted by keycols and then every column).
-- A query that raises must raise the same SQLSTATE under every setting. Each call is a statement of its own, so statement_timeout, which the
-- driver sets once, bounds every run; a run that hits it prints no line.
-- The settings are 'name=value;name=value' strings.

CREATE FUNCTION fz.apply(spec text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE kv text; g text;
BEGIN
	FOREACH g IN ARRAY ARRAY['sift.enable_agg', 'sift.agg_preagg', 'sift.agg_two_level', 'sift.agg_fuse', 'sift.agg_lookup_pushdown',
							 'sift.agg_share_lookups', 'sift.agg_dense', 'sift.enable_topn', 'sift.simd', 'sift.agg_rows', 'sift.agg_semi_lut',
							 'sift.agg_join_filter', 'max_parallel_workers_per_gather', 'parallel_setup_cost', 'parallel_tuple_cost',
							 'min_parallel_table_scan_size', 'min_parallel_index_scan_size', 'parallel_leader_participation', 'seq_page_cost',
							 'random_page_cost', 'enable_nestloop', 'enable_hashjoin', 'enable_mergejoin', 'enable_seqscan', 'enable_sort', 'sift.enable_col_scan',
							 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan'] LOOP
		EXECUTE 'RESET ' || g;
	END LOOP;
	FOREACH kv IN ARRAY string_to_array(spec, ';') LOOP
		IF kv <> '' THEN PERFORM set_config(split_part(kv, '=', 1), substr(kv, strpos(kv, '=') + 1), false); END IF;
	END LOOP;
END $$;

CREATE FUNCTION fz.start() RETURNS void LANGUAGE sql AS $$ SELECT set_config('fz.cur', '', false) $$;

-- FZ|qid|ref|status|rows: status ok, GENERR (the reference fails with a syntax or name error: the generator's fault) or ERR sqlstate
CREATE FUNCTION fz.ref(qid text, q text, spec text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE err text; n bigint := -1;
BEGIN
	PERFORM fz.apply(spec);
	err := coldiff.materialize(q, 'coldiff_ref');
	IF err IS NULL THEN EXECUTE 'SELECT count(*) FROM pg_temp.coldiff_ref' INTO n; END IF;
	PERFORM set_config('fz.err', coalesce(err, ''), false), set_config('fz.cur', qid, false);
	RETURN format('FZ|%s|ref|%s|%s', qid, CASE WHEN err IS NULL THEN 'ok' WHEN err ~ '^(42|0A)' THEN 'GENERR' ELSE 'ERR ' || left(err, 5) END, n);
END $$;

-- FZ|qid|variant name|status|used|detail: status ok or DIFF; used: the plan of the variant has Custom Scan (SiftAgg), or an index scan of ev_part (only sift_col indexes: the point family)
CREATE FUNCTION fz.var(qid text, q text, keycols int[], vname text, spec text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE err_ref text := current_setting('fz.err', true); err_got text; msg text;
BEGIN
	IF current_setting('fz.cur', true) IS DISTINCT FROM qid THEN RETURN format('FZ|%s|%s|SKIP|0|', qid, vname); END IF;
	PERFORM fz.apply(spec);
	err_got := coldiff.materialize(q, 'coldiff_got');
	IF err_ref <> '' OR err_got IS NOT NULL THEN
		IF left(err_ref, 5) IS DISTINCT FROM left(err_got, 5) THEN msg := format('error: reference %s | got %s', left(err_ref, 160), left(err_got, 160)); END IF;
	ELSE
		msg := coldiff.compare('coldiff_ref', 'coldiff_got', keycols, 1e-9, 1e-12);
	END IF;
	RETURN format('FZ|%s|%s|%s|%s|%s', qid, vname, CASE WHEN msg IS NULL THEN 'ok' ELSE 'DIFF' END,
				  CASE WHEN coldiff.explain(q) ~ 'Custom Scan \(SiftAgg\)|Index (Only )?Scan using ev_part|Bitmap Index Scan on ev_part' THEN 1 ELSE 0 END, replace(left(coalesce(msg, ''), 900), E'\n', ' '));
END $$;
