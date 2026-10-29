-- The contract of a page with its totals when the sort key has ties (`ORDER BY created_at DESC LIMIT 10 OFFSET 20`, `count(*) OVER ()`, `sum(amount) OVER ()`):
-- the statement has no tie-breaker, so the rows of one value of the key may come in another order from the top-N scan than from PostgreSQL's plan, and neither
-- is a wrong answer. What must be the same is the key of each row of the page, the totals, and that each row is a row of the table with that key. Ordered by a
-- total order (the key and the id) the two plans give the same rows in the same order. Found by the 30,000,000-row stage of the 3.6.0 record (topn_page_totals).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

-- 24,000 rows over 3,000 values of the key: every value of it is shared by 8 rows
CREATE TABLE tt (id int PRIMARY KEY, country text, amount numeric(10,2), created_at timestamp, pad text DEFAULT repeat('x', 1500));
ALTER TABLE tt ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO tt (id, country, amount, created_at)
SELECT i, CASE WHEN i % 3 = 0 THEN 'US' ELSE 'BR' END, ((hashint4(i) & 2147483647) % 30000)::numeric / 100, timestamp '2026-01-01' + (i % 3000) * interval '1 minute'
FROM generate_series(1, 24000) i;
CREATE INDEX tt_col ON tt USING sift_col (id, country, amount, created_at) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) tt;

CREATE FUNCTION tt_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- the page by the scan (a) and by PostgreSQL's plan (b): 'same rows' when the rows equal in order (a total order must give that), 'same key and totals' when the key
-- and the totals of each position equal and every row of a is the row of the table of that key (what ties must give; the order of the ties may be the same by chance,
-- so the statements with ties print 'ok' for either), 'DIFFERENT' otherwise
CREATE FUNCTION tt_page(q text, ties bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE same_rows bool; same_keys bool; valid bool; n bigint;
BEGIN
	DROP TABLE IF EXISTS tt_a, tt_b;
	PERFORM tt_mode(true);
	EXECUTE 'CREATE TEMP TABLE tt_a AS SELECT row_number() OVER () AS pos, x.* FROM (' || q || ') x';
	PERFORM tt_mode(false);
	EXECUTE 'CREATE TEMP TABLE tt_b AS SELECT row_number() OVER () AS pos, x.* FROM (' || q || ') x';
	SELECT count(*) INTO n FROM tt_b;
	same_rows := NOT EXISTS (SELECT * FROM tt_a a FULL JOIN tt_b b USING (pos) WHERE a IS DISTINCT FROM b);
	same_keys := NOT EXISTS (SELECT * FROM tt_a a FULL JOIN tt_b b USING (pos) WHERE (a.created_at, a.total, a.revenue) IS DISTINCT FROM (b.created_at, b.total, b.revenue));
	valid := (SELECT count(*) FROM tt_a a JOIN tt ON tt.id = a.id AND tt.created_at = a.created_at) = n AND (SELECT count(DISTINCT id) FROM tt_a) = n;
	RETURN CASE WHEN same_rows AND NOT ties THEN 'same rows' WHEN ties AND (same_rows OR (same_keys AND valid)) THEN 'ok' ELSE 'DIFFERENT' END || ' ' || n;
END $$;

CREATE TABLE tt_q (tag text PRIMARY KEY, ties bool, q text NOT NULL);
INSERT INTO tt_q VALUES
	('desc_total_order', false, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at DESC, id DESC LIMIT 10 OFFSET 20$q$),
	('asc_total_order',  false, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at, id LIMIT 10 OFFSET 20$q$),
	('desc_ties',        true, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at DESC LIMIT 10 OFFSET 20$q$),
	('asc_ties',         true, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at LIMIT 10 OFFSET 20$q$),
	('desc_ties_first',  true, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'BR' ORDER BY created_at DESC LIMIT 5$q$);
SELECT tag, tt_page(q, ties) FROM tt_q ORDER BY tag;

DROP TABLE tt, tt_q;
DROP FUNCTION tt_page(text, boolean), tt_mode(boolean);
DROP EXTENSION pg_sift;
