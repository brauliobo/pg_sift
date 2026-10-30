-- A search next to a condition that keeps few rows: PostgreSQL reads those rows by the btree on the key and checks the text of each; the scan walks every posting of
-- the word (about 320 instructions each) and asks the key of each. The word in an eighth of the rows and the key in 20 of them: the scan was priced 686 against
-- 766 for the bitmap plan and ran 8.23 M instructions against 4.71 M (30,000 rows 3.08 against 2.37, 250,000 rows 20.23 against 12.54). The scan is refused for
-- `cost` when the rows the key keeps are read and checked for less than the postings are walked; with no other condition, or one that keeps many rows, it is kept.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, kind text NOT NULL, amount float8 NOT NULL, deleted_at timestamptz, memo text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + (i::bigint * 7919) % 5000, date '2024-01-01' + (hashint4(i) & 1023) % 730, 'cat_' || chr(97 + i % 4), 1 + (i % 17) + 0.25 * (i % 4),
	CASE WHEN i % 37 = 0 THEN now() END,
	(ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + i % 8] || ' ' || (ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + (i * 3 + 1) % 8]
	|| ' ' || (ARRAY['relay', 'cord', 'clamp', 'bolt', 'valve'])[1 + (i * 7) % 5]
	FROM generate_series(1, 100000) i;
CREATE INDEX ev_parent ON ev (parent_id, ts, deleted_at);
CREATE INDEX ev_col ON ev USING sift_col (id, parent_id, ts, kind, amount, deleted_at);
CREATE INDEX ev_memo ON ev USING sift (memo);
CREATE TABLE tiers (id int PRIMARY KEY, tier text NOT NULL);
INSERT INTO tiers SELECT g, 'tier_' || g % 3 FROM generate_series(1, 5000) g;
VACUUM (FREEZE, ANALYZE) ev, tiers;

CREATE FUNCTION why(q text) RETURNS TABLE (used boolean, slug text) LANGUAGE sql AS
$$ SELECT e.used, e.slug FROM sift.explain_why(q) e LIMIT 1 $$;

CREATE FUNCTION diff_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO n FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	DROP TABLE r_on, r_off;
	RETURN n;
END $$;

-- the key keeps 20 rows of the 100,000: PostgreSQL's plan
SELECT * FROM why('SELECT kind, count(*) AS n FROM ev WHERE memo @@> ''switch'' AND parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1') AS few_rows_by_key;
SELECT diff_rows('SELECT kind, count(*) AS n FROM ev WHERE memo @@> ''switch'' AND parent_id = 7 AND deleted_at IS NULL GROUP BY 1 ORDER BY 1') AS differ;
-- the key keeps a fifth of the table: the scan walks the postings once
SELECT * FROM why('SELECT kind, count(*) AS n FROM ev WHERE memo @@> ''switch'' AND parent_id < 1000 GROUP BY 1 ORDER BY 1') AS many_rows_by_key;
-- no other condition: nothing to read by a key
SELECT * FROM why('SELECT kind, count(*) AS n FROM ev WHERE memo @@> ''switch'' GROUP BY 1 ORDER BY 1') AS search_alone;

-- a join clause is no key of this table: it keeps rows of the join, and the scan reads the lookup once
SELECT * FROM why('SELECT t.tier, count(*) AS n FROM ev e JOIN tiers t ON t.id = e.parent_id WHERE e.memo @@> ''switch'' GROUP BY 1 ORDER BY 1') AS join_not_a_key;

DROP FUNCTION why(text);
DROP FUNCTION diff_rows(text);
DROP TABLE ev, tiers;
DROP EXTENSION pg_sift;
