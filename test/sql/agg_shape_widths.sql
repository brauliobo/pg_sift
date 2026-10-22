-- The widths of the shapes the aggregate scan holds (UNION ALL arms, virtual leaves, lookup-joined tables, key and read columns of a join, value
-- combinations, float8 operations, leaves of a generic aggregate argument, aggregates sharing a distinct set): a shape one past a width keeps the normal
-- plan with the slug shape_width (rows_shape for the aggregates of a rows set) and the limit in the reason; one at the width runs in the scan and gives the rows of the normal plan. Each width is
-- tried at its default (where the shape is cheap to make) and at a value sift.test_limits lowers it to.
CREATE EXTENSION pg_sift;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, a int, b int, c int, d int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, i % 50, 'k' || i % 5, date '2025-01-01' + i % 6, i % 7, i % 5, i % 3, i % 2,
       jsonb_build_object('metrics', (SELECT jsonb_object_agg('m' || j, (i * j) % 10) FROM generate_series(1, 20) j),
                          'parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'score', (i * j) % 10)) FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 3000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, a, b, c, d, doc) WITH (paths = 'doc.metrics.* float8, doc.parts[]{cat text, score float8}');
-- a dimension with four key columns and eight value columns
CREATE TABLE dd (id int, a int, b int, c int, d int, v1 int, v2 int, v3 int, v4 int, v5 int, v6 int, v7 int, v8 int) WITH (autovacuum_enabled = false);
INSERT INTO dd SELECT i, i % 7, i % 5, i % 3, i % 2, i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7 FROM generate_series(0, 49) i;
VACUUM (FREEZE, ANALYZE) events, dd;

-- the rows of the scan and of the normal plan, the arm's answer and the reason
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0;
END $$;
CREATE FUNCTION probe(q text, want_arm text) RETURNS TABLE (used bool, slug text, detail text) LANGUAGE sql AS $$
	SELECT w.used, w.slug, substring(w.reason from 'the limit [a-z_]+ is [0-9]+') FROM sift.explain_why(q) w WHERE w.arm = want_arm
$$;

-- the shapes: n of something
CREATE FUNCTION shape(kind text, n int) RETURNS text LANGUAGE sql AS $$
	SELECT CASE kind
	WHEN 'virtual_leaves' THEN
		(SELECT format('SELECT kind, %s FROM events GROUP BY kind', string_agg(format('sum((doc->''metrics''->>''m%s'')::float8) AS s%s', j, j, j), ', ')) FROM generate_series(1, n) j)
	WHEN 'arms' THEN
		(SELECT format('SELECT category, sp.k, avg(score) AS value FROM generate_series(1, 20) sp(k) JOIN (%s) m ON m.parent_id = sp.k GROUP BY 1, 2',
				string_agg(format($q$SELECT e.id, e.parent_id, o.cat AS category, avg(o.score) FILTER (WHERE o.score >= %s) AS score
				           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, o.cat$q$, j), ' UNION ALL '))
		 FROM generate_series(1, n) j)
	WHEN 'joins' THEN
		(SELECT format('SELECT %s, count(*) FROM events e %s GROUP BY 1', 'd1.v1 % 3', string_agg(format('JOIN dd d%s ON d%s.id = e.parent_id', j, j), ' ') ) FROM generate_series(1, n) j)
	WHEN 'join_keys' THEN
		(SELECT format('SELECT d.v1 %% 3, count(*) FROM events e JOIN dd d ON %s GROUP BY 1', string_agg(format('d.%s = e.%s', k, k), ' AND ')) FROM unnest((ARRAY['a', 'b', 'c', 'd'])[1:n]) k)
	WHEN 'join_cols' THEN
		(SELECT format('SELECT e.kind, sum(%s) FROM events e JOIN dd d ON d.id = e.parent_id GROUP BY 1', string_agg(format('d.v%s', j), ' + ')) FROM generate_series(1, n) j)
	WHEN 'float_ops' THEN
		(SELECT format('SELECT kind, sum(%s) FROM events GROUP BY kind', 'a::float8' || string_agg(' + 1.5', '')) FROM generate_series(1, n - 1) j)
	WHEN 'rows_shared' THEN
		(SELECT format($q$SELECT sp.k, %s FROM generate_series(1, 20) sp(k)
		                  LEFT JOIN (SELECT e.id, e.parent_id, e.a, o.cat, avg(o.score) AS s
		                             FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.a, o.cat) m
		                  ON m.parent_id = sp.k GROUP BY sp.k$q$,
				string_agg(format('count(DISTINCT m.id) FILTER (WHERE m.a %% %s = 0) AS c%s', j + 1, j), ', ')) FROM generate_series(1, n) j)
	END
$$;

-- width, arm of explain_why, its default, and the value sift.test_limits lowers it to (NULL: the limit is not read through the registry yet)
CREATE TABLE widths (kind text, limit_id text, arm text, dflt int, lowered int);
INSERT INTO widths VALUES
	('virtual_leaves', 'virtual_leaves', 'single', 16, 3),
	('arms', 'arms', 'two_level', 16, 3),
	('joins', 'joins', 'single', 8, 3),
	('join_keys', 'join_keys', 'single', 3, NULL),
	('join_cols', 'join_cols', 'single', 6, 3),
	('float_ops', 'float_ops', 'single', 16, NULL),
	('rows_shared', 'rows_shared', 'rows', 32, 3);
-- float_ops and join_keys are widths too, but a shape past them does not keep the normal plan: PostgreSQL evaluates the longer float8 expression for the distinct values of its
-- leaves, and the equality columns of a join beyond the third key by the leaves they read; the rows are those of the normal plan at both sizes

-- every width at N and at N + 1: N runs in the scan (the rows of the normal plan), N + 1 keeps the normal plan and names the width
CREATE FUNCTION at_width(lower_it bool) RETURNS TABLE (kind text, n int, used bool, slug text, detail text, rows_ok bool) LANGUAGE plpgsql AS $$
DECLARE w record; q text; p record; i int;
BEGIN
	FOR w IN SELECT * FROM widths WHERE NOT lower_it OR widths.lowered IS NOT NULL ORDER BY kind LOOP
		IF lower_it THEN
			EXECUTE format('SET sift.test_limits = %L', w.limit_id || '=' || w.lowered);
		END IF;
		FOREACH i IN ARRAY ARRAY[CASE WHEN lower_it THEN w.lowered ELSE w.dflt END, CASE WHEN lower_it THEN w.lowered ELSE w.dflt END + 1] LOOP
			q := shape(w.kind, i);
			SELECT * INTO p FROM probe(q, w.arm);
			kind := w.kind; n := i; used := p.used; slug := p.slug; detail := p.detail;
			rows_ok := CASE WHEN p.used THEN same(q) END;
			RETURN NEXT;
		END LOOP;
		RESET sift.test_limits;
	END LOOP;
END $$;
SELECT * FROM at_width(false);
SELECT * FROM at_width(true);

DROP FUNCTION at_width(bool), shape(text, int), probe(text, text), same(text);
DROP TABLE widths, dd, events;
DROP EXTENSION pg_sift;
