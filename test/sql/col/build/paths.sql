-- Every way an index is built, rebuilt or maintained, on tables whose index is written in one segment and in several (a build by workers; a serial build of an ordered index that the memory budget cuts),
-- as the first index of the table and as the second one over the same columns (CREATE INDEX, CREATE INDEX
-- CONCURRENTLY, REINDEX, REINDEX CONCURRENTLY, the rewrite of the maintenance, a forced merge). A serial build writes its blocks around the buffers and its metapage last: the state of the index that
-- another read of the metapage built (the entry of REINDEX CONCURRENTLY is reloaded while it builds) left a zero page in the buffers, which the publish of a build of several segments found ("invalid
-- page layout"). After every step each index of the table verifies against the heap and answers what the heap answers.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the build paths and spools are Format 1.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET maintenance_work_mem = '16MB';

-- one: a segment; par: the segments of two participants; ord: a serial build of an ordered index cut at the memory budget (2 MB)
CREATE TABLE bp_group (grp text PRIMARY KEY, ktype text, rows int, workers int, ordered bool);
INSERT INTO bp_group
SELECT format('%s_%s', t, s), t, CASE s WHEN 'one' THEN 2000 WHEN 'par' THEN 40000 ELSE 60000 END, CASE WHEN s = 'par' THEN 2 ELSE 0 END, s = 'ord'
FROM unnest(ARRAY['int', 'text', 'uuid']) t, unnest(ARRAY['one', 'par', 'ord']) s
WHERE s = 'one' OR (s = 'par' AND t = 'text') OR (s = 'ord' AND t <> 'int');

SELECT format('CREATE TABLE bp_%s (id int PRIMARY KEY, k %s NOT NULL, w text NOT NULL, v int NOT NULL, tenant_id int NOT NULL, event_date date NOT NULL) WITH (autovacuum_enabled = false, parallel_workers = 2)', grp, ktype)
FROM bp_group ORDER BY grp \gexec
SELECT format('INSERT INTO bp_%s SELECT i, %s, ''w'' || (i %% 500) || repeat(''x'', i %% 30), i %% 100, (i * 7919 %% 400)::int, date ''2020-01-01'' + i %% 2000 FROM generate_series(1, %s) i', grp,
	CASE ktype WHEN 'int' THEN 'i' WHEN 'text' THEN 'md5(i::text) || ''-key-'' || i' ELSE 'md5(i::text)::uuid' END, rows) FROM bp_group ORDER BY grp \gexec
SELECT format('VACUUM (ANALYZE, FREEZE) bp_%s', grp) FROM bp_group ORDER BY grp \gexec

CREATE FUNCTION bp_answers(tbl regclass) RETURNS text LANGUAGE plpgsql AS
$$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (SELECT w, count(*), sum(v), min(k::text), max(k::text) FROM %s GROUP BY w) x', tbl) INTO r;
	RETURN r;
END
$$;

-- every valid index of the table: verified, and what it answers against the heap; the line names the indexes that are written in several segments
CREATE FUNCTION bp_check(tbl regclass, step text) RETURNS text LANGUAGE plpgsql AS
$$
DECLARE ix regclass; heap text; segs int; res text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	heap := bp_answers(tbl);
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR ix IN SELECT i.indexrelid FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
		WHERE i.indrelid = tbl AND a.amname = 'sift_col' AND i.indisvalid ORDER BY c.relname LOOP
		PERFORM sift.col_verify(ix, true);
		SELECT count(*) INTO segs FROM sift.col_segments(ix) WHERE kind = 'immutable';
		res := res || format(' %s=%s%s', right(ix::text, 2), CASE WHEN segs > 1 THEN 'multi' ELSE 'one' END, CASE WHEN bp_answers(tbl) = heap THEN '' ELSE ' WRONG' END);
	END LOOP;
	RETURN step || res;
END
$$;

CREATE FUNCTION bp_maintain(ix regclass) RETURNS bool LANGUAGE plpgsql AS
$$
DECLARE n int := 0;
BEGIN
	WHILE sift.col_maintain(ix) AND n < 20 LOOP n := n + 1; END LOOP;
	RETURN n < 20;
END
$$;

-- the statements of one group, in order: a path that makes the index (CREATE, CONCURRENTLY) makes both, the first and then the second; a path that rebuilds or maintains one is applied to each of two
-- indexes that were made the plain way
CREATE FUNCTION bp_steps(g text) RETURNS SETOF text LANGUAGE plpgsql AS
$$
DECLARE gr bp_group; t text := 'bp_' || g; path text; ix text; w text;
BEGIN
	SELECT * INTO gr FROM bp_group WHERE grp = g;
	w := CASE WHEN gr.ordered THEN ' WITH (key_order = ''tenant_id, event_date'', key_cut = ''off'')' ELSE '' END;
	RETURN NEXT format('SET max_parallel_maintenance_workers = %s', gr.workers);
	RETURN NEXT format('SET min_parallel_table_scan_size = %s', CASE WHEN gr.workers > 0 THEN 0 ELSE 8192 END);
	RETURN NEXT format('SET sift.test_col_segment_kb = %s', CASE WHEN gr.ordered THEN 2000 ELSE 0 END);
	FOREACH path IN ARRAY ARRAY['create', 'create concurrently'] LOOP
		RETURN NEXT format('DROP INDEX IF EXISTS %s_i1, %s_i2', t, t);
		RETURN NEXT format('CREATE INDEX %s %s_i1 ON %s USING sift_col (id, k, w, v, tenant_id, event_date)%s', CASE WHEN path = 'create' THEN '' ELSE 'CONCURRENTLY' END, t, t, w);
		RETURN NEXT format('SELECT bp_check(%L, %L)', t, g || ' ' || path || ' first');
		RETURN NEXT format('CREATE INDEX %s %s_i2 ON %s USING sift_col (k, id, w, v, tenant_id, event_date)%s', CASE WHEN path = 'create' THEN '' ELSE 'CONCURRENTLY' END, t, t, w);
		RETURN NEXT format('SELECT bp_check(%L, %L)', t, g || ' ' || path || ' second');
	END LOOP;
	FOREACH path IN ARRAY ARRAY['reindex', 'reindex concurrently', 'maintain', 'merge'] LOOP
		CONTINUE WHEN gr.ordered AND path IN ('maintain', 'merge');
		RETURN NEXT format('DROP INDEX IF EXISTS %s_i1, %s_i2', t, t);
		RETURN NEXT format('CREATE INDEX %s_i1 ON %s USING sift_col (id, k, w, v, tenant_id, event_date)%s', t, t, w);
		RETURN NEXT format('CREATE INDEX %s_i2 ON %s USING sift_col (k, id, w, v, tenant_id, event_date)%s', t, t, w);
		IF path = 'maintain' THEN
			RETURN NEXT format('UPDATE %s SET v = v + 1 WHERE id %% 7 = 0', t);
			RETURN NEXT format('DELETE FROM %s WHERE id %% 11 = 0', t);
			RETURN NEXT format('VACUUM %s', t);
		END IF;
		FOREACH ix IN ARRAY ARRAY['i1', 'i2'] LOOP
			RETURN NEXT CASE path
				WHEN 'reindex' THEN format('REINDEX INDEX %s_%s', t, ix)
				WHEN 'reindex concurrently' THEN format('REINDEX INDEX CONCURRENTLY %s_%s', t, ix)
				WHEN 'maintain' THEN format('SELECT bp_maintain(%L)', t || '_' || ix)
				ELSE format('SELECT sift.col_merge(%L, true) >= 0', t || '_' || ix) END;
			RETURN NEXT format('SELECT bp_check(%L, %L)', t, g || ' ' || path || ' ' || ix);
		END LOOP;
	END LOOP;
END
$$;

SELECT s.stmt FROM bp_group g, LATERAL bp_steps(g.grp) WITH ORDINALITY s(stmt, n) ORDER BY g.grp, s.n \gexec

SELECT format('DROP TABLE bp_%s', grp) FROM bp_group ORDER BY grp \gexec
DROP TABLE bp_group;
DROP FUNCTION bp_answers(regclass), bp_check(regclass, text), bp_maintain(regclass), bp_steps(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
