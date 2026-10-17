-- The filter shapes of the plan-choice tests (test/sql/filter_pushdown_cost.sql, filter_pushdown_clustered.sql) as tables to time
-- (bench/steps/ab.sh shapes): `shapes` holds the statement of each, run with the planner's choice and with the pushdown off and forced.
-- fc: id is the heap order, scat a permutation of it, grp 40 groups; 'common' is in 3 of 5 rows.
CREATE TABLE fc (id int PRIMARY KEY, scat int, grp int, body text, pad text);
INSERT INTO fc
SELECT i, (i * 7919) % 24001, i % 40,
	CASE WHEN i % 5 < 3 THEN repeat('common ', 1 + i % 3) ELSE '' END || 'w' || i % 97 || repeat(' filler' || i % 11, 1 + i % 13), repeat('p', 100)
FROM generate_series(1, 24000) i;
CREATE INDEX fc_body ON fc USING sift (body);
CREATE UNIQUE INDEX fc_scat ON fc (scat);
CREATE INDEX fc_grp ON fc (grp);

-- fk: ten groups of 2400 rows in the heap, labelled out of the heap's order ('g' || (c * 7 % 10)); 'common' is in 1 of 3 rows
CREATE TABLE fk (id int PRIMARY KEY, grp text, every text, body text, pad text);
INSERT INTO fk
SELECT i, 'g' || ((i - 1) / 2400 * 7 % 10), 'e' || i % 40,
	CASE WHEN i % 3 = 0 THEN repeat('common ', 1 + i % 3) ELSE '' END || 'w' || i % 97 || repeat(' filler' || i % 11, 1 + i % 13), repeat('p', 100)
FROM generate_series(1, 24000) i;
CREATE INDEX fk_body ON fk USING sift (body);
CREATE INDEX fk_grp ON fk (grp);
CREATE INDEX fk_every ON fk (every);
VACUUM ANALYZE fc, fk;

CREATE TABLE shapes (name text PRIMARY KEY, stmt text);
INSERT INTO shapes
SELECT 'fc_' || n, format('SELECT id FROM fc WHERE body @@> ''common'' AND (%s) ORDER BY sift.score(ctid) DESC LIMIT 10', flt)
FROM (VALUES ('end5', 'id BETWEEN 20401 AND 21600'), ('start5', 'id BETWEEN 1 AND 1200'), ('last70', 'id >= 7201'),
	('scat5', 'scat BETWEEN 1 AND 1200'), ('scat30', 'scat BETWEEN 1 AND 7200'), ('grp7', 'grp = 7')) v(n, flt);
INSERT INTO shapes
SELECT 'fk_' || g, format('SELECT id FROM fk WHERE body @@> ''common'' AND grp = ''%s'' ORDER BY sift.score(ctid) DESC LIMIT 10', g)
FROM (SELECT 'g' || c AS g FROM generate_series(0, 9) c) v;
INSERT INTO shapes VALUES
	('fk_every', 'SELECT id FROM fk WHERE body @@> ''common'' AND every = ''e7'' ORDER BY sift.score(ctid) DESC LIMIT 10');
