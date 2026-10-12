-- Bitmap scans over the index equal the document-level evaluation (DESIGN.md (f), (h))
CREATE EXTENSION sift;
-- the custom scan has its own tests (rank_scan); here the index answers through bitmaps
SET sift.enable_scan = off;

CREATE TABLE d (id int PRIMARY KEY, body text);
INSERT INTO d VALUES
	(1, 'The quick brown fox jumps over the lazy dog'),
	(2, 'A quick brown dog outpaces a quick red fox'),
	(3, 'Lazy afternoons: e-mail, coffee and quick naps'),
	(4, 'Brown bears eat honey; the fox is not a bear'),
	(5, 'жук and жучок crawl on the brown log'),
	(6, 'Version 2 of x86_64 builds, error-free since 2024'),
	(7, ''),
	(8, 'fox fox fox'),
	(9, NULL);
CREATE INDEX d_idx ON d USING sift (body);

-- rows found through the index and by evaluating every document
CREATE FUNCTION via_index(q text) RETURNS text LANGUAGE sql
	SET enable_seqscan = off SET enable_indexscan = off
	RETURN (SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM d WHERE body @@> q);
CREATE FUNCTION via_seq(q text) RETURNS text LANGUAGE sql
	SET enable_bitmapscan = off SET enable_indexscan = off
	RETURN (SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM d WHERE body @@> q);

SELECT q, via_index(q), via_index(q) = via_seq(q) AS same FROM (VALUES
	('fox'), ('FOX'), ('fox dog'), ('fox AND dog'), ('fox OR honey'), ('fox NOT dog'),
	('fox NOT (dog OR bear)'), ('(fox OR bear) NOT lazy'), ('brown NOT (quick NOT lazy)'),
	('{honey coffee naps}'), ('"quick brown"'), ('"brown quick"'), ('"quick brown fox"'),
	('"lazy dog"'), ('"the fox"'), ('"fox fox"'), ('e-mail'), ('"e mail"'), ('email'),
	('"brown" NOT "quick brown"'), ('fox NOT "brown fox"'),
	('qu*'), ('br?wn'), ('b*r'), ('*ear*'), ('?ox'), ('/b.*n/'), ('/(fox|dog)/'), ('/[0-9]+/'),
	('fox~'), ('fix~1'), ('quack~2'), ('bown~1'), ('jumped~2'),
	('жук'), ('ж?к'), ('жуч*'), ('жучек~1'),
	('x86'), ('x86_64'), ('2024'), ('error-free'), ('nothing'), ('fox NOT fox'), ('fox^0'),
	('{qu* /d.g/ hony~1}'), ('"a quick" OR "the quick"'), ('(a OR the) quick NOT red')
) v(q);

-- several quals on one index are ANDed; a NULL key matches nothing
SELECT id FROM d WHERE body @@> 'fox' AND body @@> 'brown' AND body @@> 'dog OR bear' ORDER BY id;
SELECT count(*) FROM d WHERE body @@> NULL::text;

-- varchar columns, expression and partial indexes
CREATE TABLE v (id int, body varchar, lang text);
INSERT INTO v VALUES (1, 'Red fox', 'en'), (2, 'Red dog', 'en'), (3, 'Roter Fuchs', 'de'), (4, 'Red cat', 'de');
CREATE INDEX v_body ON v USING sift (body);
CREATE INDEX v_lower ON v USING sift ((lower(body)));
CREATE INDEX v_en ON v USING sift (body) WHERE lang = 'en';
SET enable_seqscan = off;
SELECT id FROM v WHERE body @@> 'red' ORDER BY id;
SELECT id FROM v WHERE lower(body) @@> 'fuchs' ORDER BY id;
SELECT id FROM v WHERE body @@> 'red' AND lang = 'en' ORDER BY id;
EXPLAIN (COSTS OFF) SELECT id FROM v WHERE lower(body) @@> 'fuchs';
RESET enable_seqscan;
DROP TABLE v;

-- plans and the expansion limit
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM d WHERE body @@> 'fox';
EXPLAIN (COSTS OFF) SELECT id FROM d WHERE body @@> 'fox' AND body @@> 'dog';
EXPLAIN (COSTS OFF) SELECT id FROM d WHERE body @@> '"quick brown"';
SET sift.max_expansions = 3;
SELECT count(*) FROM d WHERE body @@> '*o*';
SELECT count(*) FROM d WHERE body @@> 'fox OR qu*';
SELECT count(*) FROM d WHERE body @@> 'fox';
RESET sift.max_expansions;
RESET enable_seqscan;

-- the planner prefers the index for a rare term and a scan for a common one
CREATE TABLE g (id int, body text);
INSERT INTO g SELECT i, 'common w' || (i % 1000) || CASE WHEN i = 17 THEN ' needle' ELSE '' END ||
	' filler filler filler filler filler filler filler filler' FROM generate_series(1, 30000) i;
CREATE INDEX g_idx ON g USING sift (body);
ANALYZE g;
EXPLAIN (COSTS OFF) SELECT id FROM g WHERE body @@> 'needle';
EXPLAIN (COSTS OFF) SELECT id FROM g WHERE body @@> 'common';
EXPLAIN (COSTS OFF) SELECT id FROM g WHERE body @@> 'w7';

-- multi-page corpus: the index agrees with evaluating each document
CREATE FUNCTION g_index(q text) RETURNS bigint LANGUAGE sql
	SET enable_seqscan = off SET enable_indexscan = off
	RETURN (SELECT count(*) FROM g WHERE body @@> q);
CREATE FUNCTION g_seq(q text) RETURNS bigint LANGUAGE sql
	SET enable_bitmapscan = off SET enable_indexscan = off
	RETURN (SELECT count(*) FROM g WHERE body @@> q);
SELECT q, g_index(q), g_index(q) = g_seq(q) AS same FROM (VALUES
	('needle'), ('common'), ('w7'), ('w7 AND needle'), ('w17 AND needle'), ('w7 OR w8'), ('w7 NOT w17'),
	('common NOT (w1 OR w2 OR w3)'), ('w1*'), ('w?7'), ('/w9.9/'), ('w77~1'), ('"common w7"'),
	('"w7 common"'), ('"filler filler"'), ('needle~1 OR w999')
) v(q);

SELECT sift.verify('g_idx');
SELECT sift.verify('d_idx');

DROP FUNCTION via_index, via_seq, g_index, g_seq;
DROP TABLE g, d;
DROP EXTENSION sift;
