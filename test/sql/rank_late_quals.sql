-- A ranked scan whose bounds tie has no threshold before it holds k rows that pass its quals, and checks every match it meets for
-- them. It is asked instead for the hits that k rows take through the quals, and checks the quals on the rows it hands out; a
-- search that falls short is followed by one sized from the rate at which the hits passed.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

CREATE FUNCTION limit_hint(q text) RETURNS int LANGUAGE sql AS $$ SELECT sift_helpers.plan_limit_hint(q) $$;

CREATE TABLE lq (id int PRIMARY KEY, body text, cc text, n int) WITH (autovacuum_enabled = off);
INSERT INTO lq SELECT i, 'common w' || i % 97 || ' x' || i % 7, CASE WHEN i % 1000 = 0 THEN 'rare' WHEN i % 10 = 0 THEN 'tenth' ELSE 'rest' END, i % 100
FROM generate_series(1, 40000) i;
CREATE INDEX lq_idx ON lq USING sift (body);
ANALYZE lq;

-- no qual the index does not answer: the LIMIT; a qual that keeps a tenth: about ten times as many hits; one that keeps a thousandth: the most
SELECT limit_hint($$SELECT id FROM lq WHERE body @@> 'common OR w5 OR x3' ORDER BY sift.score(ctid) DESC LIMIT 10$$) AS hint_none;
SELECT limit_hint($$SELECT id FROM lq WHERE body @@> 'common OR w5 OR x3' AND cc = 'tenth' ORDER BY sift.score(ctid) DESC LIMIT 10$$) BETWEEN 100 AND 400 AS hint_tenth;
SELECT limit_hint($$SELECT id FROM lq WHERE body @@> 'common OR w5 OR x3' AND cc = 'rare' ORDER BY sift.score(ctid) DESC LIMIT 10$$) AS hint_rare;
-- a query that matches few rows keeps the LIMIT: asking for a quarter of the matches is the same search
SELECT limit_hint($$SELECT id FROM lq WHERE body @@> 'w5 x3' AND cc = 'tenth' ORDER BY sift.score(ctid) DESC LIMIT 10$$) AS hint_few_matches;

-- the rows are the best ones: the same as ranking every match and checking the qual afterwards (the LIMIT is a literal of the statement)
CREATE FUNCTION late_ids(w text, c text, l int) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE
	r int[];
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM lq WHERE body @@> %L AND cc = %L AND n < 90 ORDER BY sift.score(ctid) DESC LIMIT %s) t', w, c, l) INTO r;
	RETURN r;
END $$;

SELECT count(*) AS queries,
       count(*) FILTER (WHERE got IS DISTINCT FROM want) AS different,
       count(*) FILTER (WHERE hint > l) AS asked_for_more
FROM (SELECT q.l, late_ids(q.w, q.c, q.l) AS got, limit_hint(format('SELECT id FROM lq WHERE body @@> %L AND cc = %L AND n < 90 ORDER BY sift.score(ctid) DESC LIMIT %s', q.w, q.c, q.l)) AS hint,
             (SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, s FROM (SELECT id, sift.score(ctid) s, cc, n FROM lq
                WHERE body @@> q.w ORDER BY sift.score(ctid) DESC LIMIT 1000000) a WHERE cc = q.c AND n < 90 ORDER BY s DESC, id LIMIT q.l) t) AS want
      FROM (SELECT 'common OR w' || a || ' OR x' || b AS w, c, l FROM generate_series(0, 96, 8) a, generate_series(0, 6, 2) b,
                   unnest(ARRAY['tenth', 'rare', 'rest']) c, unnest(ARRAY[1, 10, 50]) l) q) z;

DROP FUNCTION late_ids(text, text, int);
DROP FUNCTION limit_hint(text);
DROP TABLE lq;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
