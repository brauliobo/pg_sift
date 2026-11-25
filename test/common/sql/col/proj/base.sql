-- Helpers of the projection tests: the answer through the index against the answer of the heap, whether a projection was read, and a check that raises.
CREATE FUNCTION p_check(label text, ok boolean) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF ok IS DISTINCT FROM true THEN RAISE EXCEPTION 'check failed: %', label; END IF;
	RETURN 'ok';
END $$;

CREATE FUNCTION p_rows(q text, via_index boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE res text;
BEGIN
	IF via_index THEN
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
	ELSE
		SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off;
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO res;
	RETURN res;
END $$;

CREATE FUNCTION p_rows_ordered(q text, via_index boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE res text;
BEGIN
	IF via_index THEN
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
	ELSE
		SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off;
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '',''), '''') FROM (%s) r', q) INTO res;
	RETURN res;
END $$;

CREATE FUNCTION p_same(q text) RETURNS text LANGUAGE sql AS
$$ SELECT CASE WHEN p_rows(q, true) = p_rows(q, false) THEN 'same' ELSE 'DIFFERENT: ' || p_rows(q, true) || ' / ' || p_rows(q, false) END $$;

CREATE FUNCTION p_same_order(q text, heap_q text) RETURNS text LANGUAGE sql AS
$$ SELECT CASE WHEN p_rows_ordered(q, true) = p_rows_ordered(heap_q, false) THEN 'same' ELSE 'DIFFERENT: ' || p_rows_ordered(q, true) || ' / ' || p_rows_ordered(heap_q, false) END $$;

CREATE FUNCTION p_uses(q text, proj text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; found boolean := false;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		found := found OR line LIKE '%Projection: ' || proj || '%';
	END LOOP;
	RETURN found;
END $$;
