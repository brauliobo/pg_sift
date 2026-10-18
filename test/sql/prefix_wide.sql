-- A prefix with more terms than sift.max_expansions is not an error and not truncated: it matches
-- exactly the documents that have a word with the prefix (equal to sift.match) and scores like the
-- one term prefix_index would have indexed for it (what the second copy of every table holds), so
-- results do not depend on the option. Every other expansion leaf still errors above the limit.
CREATE EXTENSION pg_sift;

CREATE FUNCTION pw_doc(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT 'tk' || lpad((i % 1500)::text, 4, '0') || ' '
		|| (ARRAY['honey', 'house honey', 'tkaa tkab', 'garden', 'harbour honey'])[1 + i % 5]
		|| ' w' || i % 7 || repeat(' pad', i % 4) || CASE WHEN i % 13 = 0 THEN ' rare' ELSE '' END
$$;

-- pw: no prefix terms (the wide path); pg: prefix_index = 3 (the one-term path), same rows in the same places
CREATE TABLE pw (id int PRIMARY KEY, body text, tenant int);
INSERT INTO pw SELECT i, pw_doc(i), i % 5 FROM generate_series(1, 6000) i;
INSERT INTO pw VALUES (6001, NULL, 1), (6002, '', 1), (6003, 'tk', 1), (6004, 'TK0001 tk0001', 2);
CREATE TABLE pg (LIKE pw INCLUDING ALL);
INSERT INTO pg SELECT * FROM pw;
CREATE INDEX pw_idx ON pw USING sift (body) WITH (vacuum_segments = 128);
CREATE INDEX pg_idx ON pg USING sift (body) WITH (prefix_index = 3, vacuum_segments = 128);
VACUUM ANALYZE pw;
VACUUM ANALYZE pg;

CREATE FUNCTION qs() RETURNS SETOF text LANGUAGE sql AS $$ SELECT * FROM unnest(ARRAY[
	't*', 'tk*', 'tk0*', 'tk00*', 'tk000*', 'tk0001*', 'tk1*', 'tk14*', 'h*', 'ha*', 'ho*', 'hon*', 'w*', 'p*', 'ra*', 'x*',
	'tk* honey*', 'tk* AND NOT w3', 'NOT w3 AND tk*', '(tk0* OR h*) AND pad', 'tk0*^3 OR garden', 'h* w*', 'tk* "house honey"',
	'(tk* w*) AND (ha* OR ho*)', 'tk*^2 OR w1^3']) $$;

-- the prefixes of these have more than 512 terms or one: a leaf over the limit scores as the indexed prefix term, a lone
-- term as itself (the two coincide), and only below the limit with several terms is the score the max over them
CREATE FUNCTION wq() RETURNS SETOF text LANGUAGE sql AS $$ SELECT * FROM unnest(ARRAY[
	't*', 'tk*', 'tk0*', 'tk0001*', 'ha*', 'hon*', 'p*', 'ra*', 'x*', 'tk* honey*', 'tk* AND NOT w3', 'NOT w3 AND tk*',
	'tk0*^3 OR garden', 'tk* "house honey"', 'tk*^2 OR w1^3']) $$;

CREATE FUNCTION cnt(tbl text, q text, flt text DEFAULT 'true') RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE body @@> %L AND (%s)', tbl, q, flt) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION oracle(tbl text, q text, flt text DEFAULT 'true') RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE sift.match(body, %L) AND (%s)', tbl, q, flt) INTO n;
	RETURN n;
END $$;
-- id:score of the top k in delivery order, ties by id
CREATE FUNCTION top(tbl text, q text, k int, flt text DEFAULT 'true') RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id || '':'' || s, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id, s
		FROM (SELECT id, sift.score(ctid) s FROM %I WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC, id LIMIT %s) x) y',
		tbl, q, flt, k) INTO r;
	RETURN r;
END $$;
-- the scores alone, for tables whose rows sit in other places (ties then reach the top k by ctid)
CREATE FUNCTION metrics_a(tbl text, q text, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(s::text, '','' ORDER BY s DESC) FROM (SELECT sift.score(ctid) s FROM %I WHERE body @@> %L
		ORDER BY sift.score(ctid) DESC LIMIT %s) x', tbl, q, k) INTO r;
	RETURN r;
END $$;

SELECT sift.verify('pw_idx', true), sift.verify('pg_idx', true);
SELECT count(*) > 512 AS more_terms_than_the_limit FROM sift.tokenize((SELECT string_agg(pw_doc(i), ' ') FROM generate_series(1, 1500) i)) t WHERE t.token LIKE 'tk%';

-- the default limit: matches are those of document evaluation, through every plan
SELECT q, cnt('pw', q), oracle('pw', q) FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT count(*) AS queries, sum(cnt('pw', q)) AS hits FROM qs() q;
SET sift.enable_scan = off;
SET sift.enable_count = off;
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
RESET sift.enable_scan;
RESET sift.enable_count;
EXPLAIN (COSTS OFF) SELECT count(*) FROM pw WHERE body @@> 'tk*';
EXPLAIN (COSTS OFF) SELECT id FROM pw WHERE body @@> 'tk*' ORDER BY sift.score(ctid) DESC LIMIT 5;

-- ranked: the same documents, scores and order as the indexed prefix term, with a filter, with pushdown
SELECT q FROM wq() q WHERE top('pw', q, 25) IS DISTINCT FROM top('pg', q, 25);
SELECT top('pw', 'tk0*', 5), top('pg', 'tk0*', 5);
SELECT q FROM wq() q WHERE top('pw', q, 25, 'tenant = 2') IS DISTINCT FROM top('pg', q, 25, 'tenant = 2');
SET sift.filter_pushdown = force;
SELECT q FROM wq() q WHERE top('pw', q, 25, 'tenant = 2') IS DISTINCT FROM top('pg', q, 25, 'tenant = 2');
SELECT q FROM qs() q WHERE cnt('pw', q, 'tenant = 3') <> oracle('pw', q, 'tenant = 3');
RESET sift.filter_pushdown;
-- the limit is the number of terms a prefix may have to score as the max over them: with 1 every prefix of
-- several terms is wide, and all agree with the indexed prefix term
SET sift.max_expansions = 4;
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SET sift.max_expansions = 1;
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT q FROM qs() q WHERE top('pw', q, 25) IS DISTINCT FROM top('pg', q, 25);
SELECT q FROM qs() q WHERE top('pw', q, 25, 'tenant = 2') IS DISTINCT FROM top('pg', q, 25, 'tenant = 2');
SELECT top('pw', 'w*', 5), top('pg', 'w*', 5);
SELECT top('pw', 'h* w*', 5), top('pg', 'h* w*', 5);
SELECT cnt('pw', 'w*'), cnt('pw', 'hon*'), cnt('pw', 'honey*'), cnt('pw', 'tkaa*');
RESET sift.max_expansions;
-- wildcards, regexes and fuzzy terms above the limit are wide too (through the index: a seq scan evaluates documents and has
-- no expansion): they match what document evaluation matches, and score as one term like a prefix does
SET enable_seqscan = off;
SELECT count(*) FROM pw WHERE body @@> 't?*';
SELECT count(*) FROM pw WHERE body @@> '/tk.*/';
SELECT count(*) FROM pw WHERE body @@> 'tk* OR /tk.*/';
SELECT q FROM unnest(ARRAY['t?*', '/tk.*/', '*0001', '*k0*', '/.*(0|1)$/', 'tk0001~2', 'tk* OR /tk.*/', '*0 AND NOT w3']) q
WHERE cnt('pw', q) <> oracle('pw', q);
SET sift.max_expansions = 2;
SELECT q FROM unnest(ARRAY['t?*', '/tk.*/', '*0001', '*k0*', '/.*(0|1)$/', 'tk0001~2', 'tk* OR /tk.*/', '*0 AND NOT w3', 'hon?y', 'hony~1']) q
WHERE cnt('pw', q) <> oracle('pw', q);
RESET sift.max_expansions;
SET sift.max_expansions = 5000;
SELECT count(*) FROM pw WHERE body @@> '/tk.*/';
RESET sift.max_expansions;
RESET enable_seqscan;

-- the parallel plans return what the serial ones do
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SET sift.parallel_scan = on;
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT q FROM wq() q WHERE top('pw', q, 25) IS DISTINCT FROM top('pg', q, 25);
RESET sift.parallel_scan;
RESET max_parallel_workers_per_gather;

-- mutable segments, deletes, updates, promotion, merge: counts agree with document evaluation and the ranking with the
-- indexed prefix term, whatever the layout (the new rows sit in a mutable segment until promoted)
INSERT INTO pw SELECT i, pw_doc(i), i % 5 FROM generate_series(7001, 7300) i;
INSERT INTO pg SELECT * FROM pw WHERE id >= 7001;
SELECT count(*) FILTER (WHERE kind = 'mutable') AS mutable, count(*) FILTER (WHERE kind = 'immutable') AS immutable FROM sift.segments('pw_idx');
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT q FROM wq() q WHERE top('pw', q, 25) IS DISTINCT FROM top('pg', q, 25);
DELETE FROM pw WHERE id % 7 = 0;
DELETE FROM pg WHERE id % 7 = 0;
UPDATE pw SET body = 'tk0001 tk9999 alpha' WHERE id = 8;
UPDATE pg SET body = 'tk0001 tk9999 alpha' WHERE id = 8;
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT sift.promote('pw_idx') IS NOT NULL AS promoted;
SELECT sift.promote('pg_idx') IS NOT NULL AS promoted;
VACUUM pw;
VACUUM pg;
SELECT bool_or(dirty) AS dirty, count(*) AS immutable FROM sift.segments('pw_idx') WHERE kind = 'immutable';
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT q FROM wq() q WHERE top('pw', q, 25) IS DISTINCT FROM top('pg', q, 25);
SELECT cnt('pw', 'tk0001*'), cnt('pw', 'alp*'), cnt('pw', 'tk9*');
SELECT sift.merge('pw_idx', true) IS NOT NULL AS merged;
SELECT sift.merge('pg_idx', true) IS NOT NULL AS merged;
SELECT q FROM qs() q WHERE cnt('pw', q) <> oracle('pw', q);
SELECT q FROM wq() q WHERE top('pw', q, 25) IS DISTINCT FROM top('pg', q, 25);
SELECT sift.verify('pw_idx', true), sift.verify('pg_idx', true);

-- several segments weigh a wide prefix by its document frequency over all of them
CREATE TABLE ps (id int PRIMARY KEY, body text, tenant int);
INSERT INTO ps SELECT * FROM pw;
CREATE INDEX ps_idx ON ps USING sift (body) WITH (build_segments = 3);
SELECT count(*) AS segments FROM sift.segments('ps_idx') WHERE kind = 'immutable';
SELECT q FROM qs() q WHERE cnt('ps', q) <> oracle('ps', q);
SELECT q FROM qs() q WHERE metrics_a('ps', q, 25) IS DISTINCT FROM metrics_a('pw', q, 25);
SET sift.max_expansions = 1;
SELECT q FROM qs() q WHERE metrics_a('ps', q, 25) IS DISTINCT FROM metrics_a('pw', q, 25);
RESET sift.max_expansions;

-- a field's words: name:tk* is a wide prefix of the section's own terms
CREATE FUNCTION sections(a text, b text) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT coalesce(a, '') || chr(29) || coalesce(b, '') $$;
CREATE TABLE pf (id int PRIMARY KEY, a text, b text);
INSERT INTO pf SELECT i, pw_doc(i), pw_doc(i + 1) FROM generate_series(1, 3000) i;
CREATE INDEX pf_idx ON pf USING sift (sections(a, b)) WITH (fields = 'a,b');
SELECT count(*) FROM pf WHERE sections(a, b) @@> 'a:tk*';
SELECT count(*) FROM pf WHERE sift.match(sections(a, b), 'a:tk*');
SELECT count(*) FROM pf WHERE sections(a, b) @@> 'a:tk0* AND NOT b:tk01*';
SELECT count(*) FROM pf WHERE sift.match(sections(a, b), 'a:tk0* AND NOT b:tk01*');
SELECT count(*) FROM (SELECT id FROM pf WHERE sections(a, b) @@> '(tk* OR a:tk*)^2 AND a:{ha* ho*}' ORDER BY sift.score(ctid) DESC, id LIMIT 10) x;

-- through a partitioned parent: each partition has its own statistics
CREATE TABLE pp (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (0) TO (3000);
CREATE TABLE pp2 PARTITION OF pp FOR VALUES FROM (3000) TO (6000);
INSERT INTO pp SELECT i, pw_doc(i) FROM generate_series(1, 5999) i;
CREATE INDEX pp_idx ON pp USING sift (body);
VACUUM ANALYZE pp;
SELECT q, (SELECT count(*) FROM pp WHERE body @@> q) AS parent, (SELECT count(*) FROM pp WHERE sift.match(body, q)) AS oracle FROM qs() q
WHERE (SELECT count(*) FROM pp WHERE body @@> q) <> (SELECT count(*) FROM pp WHERE sift.match(body, q));
SELECT count(*) FROM (SELECT id FROM pp WHERE body @@> 'tk*' ORDER BY sift.score(ctid) DESC LIMIT 10) x;

DROP TABLE pw, pg, ps, pf, pp;
DROP FUNCTION qs(), wq(), cnt(text, text, text), oracle(text, text, text), top(text, text, int, text), metrics_a(text, text, int),
	pw_doc(int), sections(text, text);
DROP EXTENSION pg_sift;
