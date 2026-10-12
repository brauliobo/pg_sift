-- Self-checking helpers for the isolation specs, loaded by scripts/isolation.sh
-- after test/sql/reference.sql. Steps print only booleans, so the expected
-- output does not depend on sift internals.

-- Drains up to n rows of cursor cur (columns id [, score]) into the session's
-- temp table fetched(n serial, id int, score real); returns the rows read
CREATE FUNCTION iso_fetch(cur refcursor, n int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE
	r record;
	i int := 0;
BEGIN
	WHILE i < n LOOP
		FETCH cur INTO r;
		EXIT WHEN NOT FOUND;
		INSERT INTO pg_temp.fetched (id, score) VALUES (r.id, (to_jsonb(r) ->> 'score')::real);
		i := i + 1;
	END LOOP;
	RETURN i;
END $$;

-- The drained rows are exactly the reference matches under the current
-- snapshot, without duplicates, in non-increasing score order
CREATE FUNCTION iso_cursor_ok(tbl regclass, q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	ref int[];
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY id) FROM %s WHERE sift_ref.match(body, %L)', tbl, q) INTO ref;
	RETURN (SELECT array_agg(id ORDER BY id) FROM pg_temp.fetched) IS NOT DISTINCT FROM ref
		AND NOT EXISTS (SELECT 1 FROM pg_temp.fetched a JOIN pg_temp.fetched b ON b.n = a.n + 1 WHERE b.score > a.score);
END $$;

-- An index scan (seq scan disabled) returns exactly the reference matches
CREATE FUNCTION iso_agree(tbl regclass, q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	plan text;
	got tid[];
	ref tid[];
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	EXECUTE format('EXPLAIN SELECT ctid FROM %s WHERE body @@> %L', tbl, q) INTO plan;
	IF current_setting('iso.mode', true) = 'sift' AND plan LIKE 'Seq Scan%' THEN
		RAISE EXCEPTION 'sift index not used: %', plan;
	END IF;
	EXECUTE format('SELECT array_agg(ctid ORDER BY ctid) FROM %s WHERE body @@> %L', tbl, q) INTO got;
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('SELECT array_agg(ctid ORDER BY ctid) FROM %s WHERE sift_ref.match(body, %L)', tbl, q) INTO ref;
	RETURN got IS NOT DISTINCT FROM ref;
END $$;

CREATE FUNCTION iso_verify(idx regclass) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.verify(idx, true);
	RETURN true;
END $$;
