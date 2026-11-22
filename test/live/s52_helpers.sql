-- s52_helpers.sql - the check helpers of the SQL suites: s52_ok, s52_hash (rows of a query with sift.enable_agg on or off), s52_plan, s52_check.

SET client_min_messages = warning;

CREATE TEMP TABLE checks (name text PRIMARY KEY, ok boolean NOT NULL);

CREATE FUNCTION s52_ok(n text, r boolean) RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
  INSERT INTO checks VALUES (n, r);
  RAISE NOTICE '% | %', n, r;
END $f$;

CREATE FUNCTION s52_hash(q text, agg boolean) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE h text;
BEGIN
  PERFORM set_config('sift.enable_agg', agg::text, true);
  PERFORM set_config('extra_float_digits', '-4', true);
  EXECUTE 'SELECT md5(coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO h;
  RETURN h;
END $f$;

CREATE FUNCTION s52_plan(q text) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE r text; p text := '';
BEGIN
  PERFORM set_config('sift.enable_agg', 'on', true);
  PERFORM set_config('enable_sort', 'off', true);
  PERFORM set_config('enable_hashagg', 'off', true);
  FOR r IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP p := p || r || E'\n'; END LOOP;
  RETURN p;
END $f$;

CREATE FUNCTION s52_check(name text, q text, node text DEFAULT NULL, also text DEFAULT NULL) RETURNS void LANGUAGE plpgsql AS $f$
DECLARE p text := s52_plan(q);
BEGIN
  PERFORM s52_ok(name || ': same rows', s52_hash(q, true) = s52_hash(q, false));
  IF node IS NOT NULL THEN PERFORM s52_ok(name || ': ' || node, position(node IN p) > 0); END IF;
  IF also IS NOT NULL THEN PERFORM s52_ok(name || ': ' || also, position(also IN p) > 0); END IF;
END $f$;
