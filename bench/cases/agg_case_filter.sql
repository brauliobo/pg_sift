-- a pivot of sums over CASE WHEN p THEN x ELSE 0 END: the scan computes the sum of x under a test of p (an aggregate with a filter) instead of the CASE for every row
-- label: `SELECT grp, sum(CASE WHEN st = 'paid' THEN amount ELSE 0 END), sum(CASE WHEN st = 'open' THEN amount ELSE 0 END) FROM cf_fact GROUP BY grp` over 600,000 rows
-- ordered: yes
--@ setup
CREATE TABLE cf_fact AS SELECT g AS id, g % 53 AS grp, (array['new', 'open', 'paid', 'shipped', 'closed'])[1 + (g * 7 / 5) % 5] AS st,
       CASE WHEN g % 20 = 0 THEN NULL ELSE ((g::bigint * 7919 % 100000) / 100.0)::float8 END AS amount FROM generate_series(1, {rows} * 3) g;
CREATE INDEX cf_fact_col ON cf_fact USING sift_col (id, grp, st, amount);
ANALYZE cf_fact
--@ pg
SELECT grp, sum(CASE WHEN st = 'paid' THEN amount ELSE 0 END) AS paid, sum(CASE WHEN st = 'open' THEN amount ELSE 0 END) AS open FROM cf_fact GROUP BY grp ORDER BY grp
