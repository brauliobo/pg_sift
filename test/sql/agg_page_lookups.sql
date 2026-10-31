-- The one-row lookups of a page (LEFT JOIN LATERAL of a primary key, of an aggregate without GROUP BY, of a LIMIT 1, a LEFT JOIN on a unique key) are made for the rows
-- of the page, not for every row that matches before ORDER BY .. LIMIT. The joins that can change the rows (a join that fans out, an inner join, a condition
-- or an order on their columns) stay where they are, and the lookups above them wait. page_only: the left joins of the plan produce at most the rows of the page.
-- Each statement gives the rows of the plan without the extension, whatever the plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE dim_a (id int PRIMARY KEY, name text NOT NULL, kind text NOT NULL);
INSERT INTO dim_a SELECT g, 'a' || g, 'k' || g % 4 FROM generate_series(1, 300) g;
CREATE TABLE dim_b (id int PRIMARY KEY, url text NOT NULL);
INSERT INTO dim_b SELECT g, 'u' || g FROM generate_series(1, 2000) g;
CREATE TABLE links_a (id int PRIMARY KEY, dim_a_id int NOT NULL, label text NOT NULL);
INSERT INTO links_a SELECT g, 1 + g % 300, 'l' || g FROM generate_series(1, 900) g;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, dim_b_id int, event_date date NOT NULL, status smallint NOT NULL, doc jsonb, deleted_at timestamp) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + (g * 7919) % 300, CASE WHEN g % 9 <> 0 THEN 1 + g % 2000 END, date '2025-01-01' + (g * 31) % 300, g % 5,
       jsonb_build_object('lang', 'l' || g % 3, 'notes', jsonb_build_array(jsonb_build_object('score', g % 7), jsonb_build_object('score', g % 5))),
       CASE WHEN g % 23 = 0 THEN timestamp '2025-06-01' END FROM generate_series(1, 60000) g;
CREATE INDEX events_col ON events USING sift_col (dim_a_id, event_date, status, deleted_at);
VACUUM (FREEZE, ANALYZE) events, dim_a, dim_b, links_a;

CREATE FUNCTION page(extra text, tail text, lim text DEFAULT 'LIMIT 3') RETURNS text LANGUAGE sql AS $$
	SELECT 'SELECT e.id, e.event_date, e.status, a.name, b.url, s.summary FROM events e JOIN dim_a ON dim_a.id = e.dim_a_id ' || extra ||
	       ' WHERE e.deleted_at IS NULL AND e.dim_a_id IN (SELECT id FROM dim_a WHERE kind <> ''k0'') ' || tail || ' ORDER BY e.event_date DESC NULLS LAST, e.id ' || lim
$$;
CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
	('lateral key lookups and an aggregate', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true
		LEFT JOIN LATERAL (SELECT json_build_object('lang', e.doc ->> 'lang', 'avg', pl.avg) AS summary
		                   FROM (SELECT avg(n.score) AS avg FROM jsonb_to_recordset(e.doc -> 'notes') AS n(score float)) pl) s ON true$x$, '')),
	('left join on a key', page($x$
		LEFT JOIN dim_a a ON a.id = e.dim_a_id LEFT JOIN dim_b b ON b.id = e.dim_b_id LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, '')),
	('lateral limit 1', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.kind = e.status::text ORDER BY dim_a.id LIMIT 1) a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, '')),
	('offset', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, '', 'LIMIT 3 OFFSET 400')),
	('one lookup, the rest fans out', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true
		LEFT JOIN links_a l ON l.dim_a_id = e.dim_a_id LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, '')),
	('lookup of a column that is not a key', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.kind = 'k1') a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, '')),
	('inner lateral', page($x$
		INNER JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id AND dim_a.kind <> 'k1') a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, '')),
	('condition on a looked up column', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, 'AND b.url <> ''u7''')),
	('order by a looked up column', 'SELECT e.id, a.name, b.url FROM events e LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true ' ||
		'LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true WHERE e.deleted_at IS NULL ORDER BY b.url NULLS LAST, e.id LIMIT 3'),
	('no limit', page($x$
		LEFT JOIN LATERAL (SELECT dim_a.name FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true
		LEFT JOIN LATERAL (SELECT bb.url FROM dim_b bb WHERE bb.id = e.dim_b_id) b ON true LEFT JOIN LATERAL (SELECT NULL::text AS summary) s ON true$x$, 'AND e.id < 3000', ''));

-- the rows the outer joins of the plan produce (the lookups run for them), and whether the rows are the plan without the extension's
CREATE FUNCTION lookup_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO p;
	RETURN coalesce((SELECT max((n ->> 'Actual Rows')::numeric * coalesce((n ->> 'Actual Loops')::numeric, 1))::bigint
	                 FROM jsonb_path_query(p, 'strict $.**.Plans[*] ? (@."Join Type" == "Left")') n), 0);
END $$;
CREATE FUNCTION rows_equal(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT string_agg(r::text, ''|'' ORDER BY ord) FROM (SELECT r, row_number() OVER () AS ord FROM (%s) r) x', q) INTO a;
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE format('SELECT string_agg(r::text, ''|'' ORDER BY ord) FROM (SELECT r, row_number() OVER () AS ord FROM (%s) r) x', q) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
SELECT name, lookup_rows(q) <= 3 AS page_only, rows_equal(q) AS rows_equal FROM forms ORDER BY name;

DROP FUNCTION page(text, text, text), lookup_rows(text), rows_equal(text);
DROP TABLE forms, events, dim_a, dim_b, links_a;
DROP EXTENSION pg_sift;
