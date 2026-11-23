# sift.score() in UPDATE and FOR UPDATE on a row a concurrent transaction changed: EvalPlanQual hands the scan the new row version, which is matched and
# scored from its own text, on a table with a sift index (SiftScan) and on one without (SiftScoreScan). A version that no longer matches is no row.
# requires: build scan score

setup
{
	CREATE TABLE epq_idx (id int PRIMARY KEY, body text, s real);
	CREATE TABLE epq_raw (id int PRIMARY KEY, body text, s real);
	INSERT INTO epq_idx SELECT g, CASE g % 4 WHEN 1 THEN 'wire fraud' WHEN 2 THEN 'wire cable' WHEN 3 THEN 'cable only' ELSE 'quiet night' END
		FROM generate_series(1, 40) g;
	INSERT INTO epq_raw SELECT id, body FROM epq_idx;
	CREATE INDEX epq_idx_idx ON epq_idx USING sift (body);
	SELECT sift.parse_query('wire');
}

teardown
{
	DROP TABLE epq_idx, epq_raw;
}

session s1
step s1ui	{ UPDATE epq_idx SET s = sift.score(ctid) WHERE body @@> 'wire'; }
step s1ur	{ UPDATE epq_raw SET s = sift.score(ctid) WHERE body @@> 'wire'; }
step s1uc	{ UPDATE epq_raw SET s = sift.score(ctid) WHERE body @@> 'wire' AND sift.score(ctid) < 0.9; }
step s1li	{ SELECT id, sift.score(ctid) > 0.9 AS best FROM epq_idx WHERE body @@> 'wire' ORDER BY id FOR UPDATE; }
step s1lr	{ SELECT id, sift.score(ctid) > 0.9 AS best FROM epq_raw WHERE body @@> 'wire' ORDER BY id FOR UPDATE; }
step s1r	{ SELECT tbl, count(*) AS updated, count(*) FILTER (WHERE s > 0.9) AS best, bool_or(id = 1) AS first_row
			  FROM (SELECT 'idx' AS tbl, id, s FROM epq_idx UNION ALL SELECT 'raw', id, s FROM epq_raw) t WHERE s IS NOT NULL GROUP BY tbl ORDER BY tbl; }

session s2
step s2b	{ BEGIN; }
step s2m	{ UPDATE epq_idx SET body = CASE id WHEN 1 THEN 'cable' ELSE 'wire wire wire wire' END WHERE id IN (1, 2);
			  UPDATE epq_raw SET body = CASE id WHEN 1 THEN 'cable' ELSE 'wire wire wire wire' END WHERE id IN (1, 2); }
step s2c	{ COMMIT; }

permutation s2b s2m s1ui s2c s1r
permutation s2b s2m s1ur s2c s1r
permutation s2b s2m s1uc s2c s1r
permutation s2b s2m s1li s2c
permutation s2b s2m s1lr s2c
