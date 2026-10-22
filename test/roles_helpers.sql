-- Shared by roles_*.sql: runs a table of "role | statement" lines as ordinary roles and reports, per line, what that role got.
--
-- rr_roles_drop(prefix) removes the roles of an earlier run that died (roles are cluster-wide, a database is not).
-- rr_run(script) runs every line "role | statement" under SET LOCAL SESSION AUTHORIZATION role, each in a subtransaction, one result row per line:
--   ok                    the statement ran (DML: with the row count); "!" in front of a SELECT runs it and drops its rows
--   ok: r1 ; r2           a SELECT, VALUES, WITH, TABLE, EXECUTE, FETCH or SHOW statement: its rows as text, in order
--   ok: plan NODE, ...    an EXPLAIN statement: the sift and scan nodes of the plan (no costs, no timings)
--   ERROR: message        the statement failed
-- rr_matrix(roles, calls) runs every statement of calls (one per line, as for rr_run) as each of the roles, and groups the roles that got the same
-- result: one row per statement and result, with the roles that got it in the order they are listed.
-- Blank lines and lines that start with "--" are skipped; "role" may be postgres, the superuser that runs the test. A statement that cannot run in a
-- function (VACUUM, CREATE INDEX CONCURRENTLY, COPY STDIN/STDOUT) is written in the test file under SET SESSION AUTHORIZATION.
SET client_min_messages = warning;

CREATE FUNCTION rr_roles_drop(prefix text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r name;
BEGIN
	FOR r IN SELECT rolname FROM pg_roles WHERE starts_with(rolname, prefix) LOOP
		EXECUTE format('DROP OWNED BY %I CASCADE', r);
		EXECUTE format('DROP ROLE %I', r);
	END LOOP;
END $$;

CREATE FUNCTION rr_run(script text) RETURNS TABLE(n int, role text, stmt text, result text) LANGUAGE plpgsql AS $$
DECLARE
	line text; plan text; m text[]; nodes text[]; rows text; cnt bigint; quiet bool; rec record;
BEGIN
	n := 0;
	FOREACH line IN ARRAY string_to_array(script, E'\n') LOOP
		CONTINUE WHEN btrim(line) = '' OR btrim(line) LIKE '--%';
		n := n + 1;
		role := btrim(split_part(line, '|', 1));
		stmt := btrim(substr(line, strpos(line, '|') + 1));
		quiet := stmt LIKE '!%'; rows := NULL;
		IF quiet THEN stmt := substr(stmt, 2); END IF;
		BEGIN
			EXECUTE format('SET LOCAL SESSION AUTHORIZATION %I', role);
			IF quiet THEN
				EXECUTE stmt;
				result := 'ok';
			ELSIF stmt ~* '^(select|values|with|table|execute|fetch|show)\M' THEN
				FOR rec IN EXECUTE stmt LOOP rows := concat_ws(' ; ', rows, rec::text); END LOOP;
				result := 'ok: ' || coalesce(rows, '(no rows)');
			ELSIF stmt ~* '^explain\M' THEN
				nodes := '{}';
				FOR plan IN EXECUTE stmt LOOP
					m := regexp_match(plan, '(Custom Scan \(\w+\)|Index (?:Only )?Scan using \w+|Bitmap Index Scan on \w+|Seq Scan on \w+)');
					IF m IS NOT NULL THEN nodes := nodes || m[1]; END IF;
				END LOOP;
				SELECT 'ok: plan ' || coalesce(string_agg(DISTINCT x, ', ' ORDER BY x), '(none)') INTO result FROM unnest(nodes) x;
			ELSE
				EXECUTE stmt;
				GET DIAGNOSTICS cnt = ROW_COUNT;
				result := CASE WHEN stmt ~* '^(insert|update|delete|merge)\M' THEN format('ok (%s)', cnt) ELSE 'ok' END;
			END IF;
		EXCEPTION WHEN OTHERS THEN
			result := 'ERROR: ' || SQLERRM;
		END;
		RESET SESSION AUTHORIZATION;
		RETURN NEXT;
	END LOOP;
END $$;

CREATE FUNCTION rr_matrix(roles text[], calls text) RETURNS TABLE(stmt text, result text, by_role text) LANGUAGE sql AS $$
	SELECT r.stmt, r.result, string_agg(r.role, ', ' ORDER BY r.n)
	FROM rr_run((SELECT string_agg(ro || ' | ' || c, E'\n' ORDER BY ci, ri)
				 FROM unnest(string_to_array(calls, E'\n')) WITH ORDINALITY c(c, ci)
					 CROSS JOIN unnest(roles) WITH ORDINALITY ro(ro, ri)
				 WHERE btrim(c) <> '' AND btrim(c) NOT LIKE '--%')) r
	GROUP BY r.stmt, r.result ORDER BY min(r.n)
$$;
