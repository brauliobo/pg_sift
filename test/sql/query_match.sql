-- Document evaluation without an index: sift.match / @@> (DESIGN.md (g), (h))
CREATE EXTENSION sift;

CREATE TABLE d (id int PRIMARY KEY, body text);
INSERT INTO d VALUES
	(1, 'The quick brown fox jumps over the lazy dog'),
	(2, 'A quick brown dog outpaces a quick red fox'),
	(3, 'Lazy afternoons: e-mail, coffee and quick naps'),
	(4, 'Brown bears eat honey; the fox is not a bear'),
	(5, 'жук and жучок crawl on the brown log'),
	(6, 'Version 2 of x86_64 builds, error-free since 2024'),
	(7, ''),
	(8, 'fox fox fox');

CREATE FUNCTION hits(q text) RETURNS text LANGUAGE sql
	RETURN (SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM d WHERE body @@> q);

-- every construct, plus the canonical form must match the same rows
SELECT q, hits(q), hits(q) = hits(sift.parse_query(q)) AS canon_equal FROM (VALUES
	('fox'), ('FOX'), ('fox dog'), ('fox AND dog'), ('fox OR honey'), ('fox NOT dog'),
	('fox NOT (dog OR bear)'), ('(fox OR bear) NOT lazy'), ('brown NOT (quick NOT lazy)'),
	('{honey coffee naps}'), ('"quick brown"'), ('"brown quick"'), ('"quick brown fox"'),
	('"lazy dog"'), ('"the fox"'), ('"fox fox"'), ('"fox fox fox fox"'), ('e-mail'),
	('"e mail"'), ('email'), ('"brown" NOT "quick brown"'), ('fox NOT "brown fox"'),
	('qu*'), ('quick*'), ('br?wn'), ('b*r'), ('b*r*'), ('*ear*'), ('?ox'), ('l?zy'),
	('/b.*n/'), ('/(fox|dog)/'), ('/[0-9]+/'), ('/x86/'), ('/x86.*/'), ('/(?i)FOX/'),
	('fox~'), ('fix~1'), ('fxo~1'), ('quack~1'), ('quack~2'), ('qiuck~1'), ('brwn~1'),
	('bown~1'), ('frown~1'), ('ofx~2'), ('jumped~2'), ('dg~2'), ('do~1'), ('\(lazzy\)~1'), ('x-86~1'),
	('жук'), ('ж?к'), ('жуч*'), ('жучек~1'), ('жучок~2'),
	('x86'), ('x86_64'), ('"x86 64"'), ('2024'), ('error-free'),
	('--'), ('fox --'), ('-- NOT fox'), ('fox NOT fox'), ('fox^0'), ('fox OR NOT dog AND bear'),
	('{qu* /d.g/ hony~1}'), ('"a quick" OR "the quick"'), ('(a OR the) quick NOT red')
) v(q);

-- operator and function agree; NULLs are strict
SELECT count(*) FROM d WHERE sift.match(body, 'fox') <> (body @@> 'fox');
SELECT NULL::text @@> 'fox', 'fox' @@> NULL::text;

-- the query is parsed once per scan and per distinct query text
SELECT q.s, count(*) FROM d, (VALUES ('fox'), ('quick'), ('"quick brown"')) q(s)
	WHERE d.body @@> q.s GROUP BY q.s ORDER BY q.s;

-- errors surface at execution
SELECT count(*) FROM d WHERE body @@> 'fox OR';
SELECT 'abc' @@> '/a(/';

-- query literals fold like the index: case, accents, compatibility forms
SELECT 'Café Straße' @@> 'CAFE strasse', 'Café' @@> 'cafè', 'Café' @@> 'CAF?', 'Ｃａｆé' @@> 'café*',
	   'Crème brûlée' @@> '"creme brulee"', 'Crème' @@> 'crãme~1', 'Crème' @@> 'krème~1', 'naïve' @@> '/na.ve/';

-- long documents and terms
SELECT repeat('lorem ipsum ', 100000) || 'needle' @@> '"ipsum needle"',
	   repeat('lorem ipsum ', 100000) @@> 'needle',
	   repeat('z', 1000) @@> repeat('z', 255),
	   repeat('z', 1000) @@> (repeat('z', 300) || '*'),
	   repeat('z', 1000) @@> (repeat('z', 254) || '?');

DROP FUNCTION hits;
DROP TABLE d;
DROP EXTENSION sift;
