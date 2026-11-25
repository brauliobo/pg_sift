CREATE EXTENSION pg_sift;
CREATE FUNCTION toks(doc text, stemmer text DEFAULT NULL, c bool DEFAULT NULL, a bool DEFAULT NULL)
  RETURNS text LANGUAGE sql
  AS $$ SELECT string_agg(token, ' ' ORDER BY pos) FROM sift.tokenize(doc, NULL, stemmer, c, a) $$;

-- positions and character offsets
SELECT *, substr(d, start_offset + 1, end_offset - start_offset) AS source
  FROM (VALUES ('The  Quick-brown fox jumps over the lazy dog.')) v(d), sift.tokenize(d);
SELECT *, substr(d, start_offset + 1, end_offset - start_offset) AS source
  FROM (VALUES ('Crème brûlée à São Paulo')) v(d), sift.tokenize(d);

-- empty, NULL, separators only
SELECT count(*) FROM sift.tokenize('');
SELECT count(*) FROM sift.tokenize(NULL);
SELECT count(*) FROM sift.tokenize(' .,;:!?-_/\()[]{}<>"''`~@#$%^&*+=| ');

-- apostrophes, hyphens, underscores, periods and numbers separate
SELECT d, toks(d) FROM (VALUES
  ('don''t'), ('O''Neil''s'), ('John’s book'), ('rock''n''roll'), ('l''amour'), ('ʼokina ʻokina'),
  ('e-mail snake_case U.S.A.'), ('3.14 1,000 2024-10-11 10:30'), ('mp3 covid19 x86_64 C++ C# .NET'),
  ('user@example.com https://a.b/c?d=e')) v(d);

-- case folding (full Unicode case folding)
SELECT d, toks(d) FROM (VALUES
  ('HELLO World'), ('STRASSE Straße ẞ'), ('ΣΊΣΥΦΟΣ σίσυφος'), ('ǅemal Ǆ'),
  ('İstanbul ISTANBUL ıi'), ('ﬁnancial ﬀ'), ('ＦＵＬＬ　ｗｉｄｔｈ ０１２'), ('ΜΆΪΟΣ'),
  ('ĞÜŞÇÖ'), ('ԱՐԱՄ'), ('ᏣᎳᎩ')) v(d);

-- accent and compatibility folding
SELECT d, toks(d) FROM (VALUES
  ('Crème Brûlée NAÏVE Ångström'), ('Łódź Øresund Æsir Œuvre þorn Đorđe'), ('x² H₂O ① ½'),
  ('Ελληνικά ἀρχή ᾳ'), ('Москва Ёлка Йошкар-Ола їжак ў'), ('mañana Nguyễn'),
  ('مُحَمَّد'), ('עִבְרִית'), ('ﷲ'), ('ｶﾀｶﾅ'), ('㎏ ℌ ℃')) v(d);

-- decomposed input folds per code point
SELECT toks(E'Crème'), toks(E'́abc'), toks(E'́');

-- folding modes: preserve, case only, accent only
SELECT d, toks(d, NULL, false, false) AS preserve, toks(d, NULL, true, false) AS case_only,
       toks(d, NULL, false, true) AS accent_only
  FROM (VALUES ('Crème BRÛLÉE'), ('Straße ﬁ'), ('ΣΊΣΥΦΟΣ'), ('ＡＢＣ')) v(d);

-- scripts: Han and Hiragana per character, other scripts as runs
SELECT d, toks(d) FROM (VALUES
  ('東京タワーに行きました'), ('ひらがな カタカナ'), ('中文分词test測試'), ('한국어 텍스트'),
  ('ภาษาไทย'), ('हिन्दी भाषा'), ('বাংলা'), ('ქართული'), ('𠀀𠀁 々')) v(d);

-- emoji: one token per pictograph, modifiers dropped, flags pair up
SELECT d, toks(d) FROM (VALUES
  ('👍🏽👍'), ('I ❤️ NY'), ('👨‍👩‍👧'), ('🇧🇷🇺🇸🇫'), ('1️⃣ #️⃣'), ('™ © ®'), ('👍word👍'),
  ('🏴󠁧󠁢󠁳󠁣󠁴󠁿')) v(d);

-- default ignorables join, zero width space separates
SELECT toks(E'co­op'), toks(E'zero​width'), toks(E'zw‍j'), toks(E'﻿bom'),
       toks(E'a‏b');

-- a token over SIFT_MAX_TERM_BYTES is its start (cut at a character boundary), U+FFFF and a hash of all of it
SELECT pos, length(token), octet_length(token), end_offset
  FROM sift.tokenize(repeat('a', 300) || ' ' || repeat('é', 200) || ' ' || repeat('東', 10) || ' x');
SELECT octet_length(token), length(token)
  FROM sift.tokenize(repeat('é', 200), case_folding => false, accent_folding => false);
SELECT octet_length(token) FROM sift.tokenize('a' || repeat('ﬃ', 100));

-- properties over random strings of mixed scripts (deterministic seed)
SELECT setseed(0.42);
CREATE TEMP TABLE rnd AS
  SELECT i, (SELECT string_agg(chr(CASE WHEN r < 0.3 THEN 97 + (random() * 25)::int
                                        WHEN r < 0.4 THEN 32
                                        WHEN r < 0.5 THEN 192 + (random() * 300)::int
                                        WHEN r < 0.6 THEN 768 + (random() * 111)::int
                                        WHEN r < 0.7 THEN 19968 + (random() * 200)::int
                                        WHEN r < 0.8 THEN 127744 + (random() * 300)::int
                                        WHEN r < 0.9 THEN 33 + (random() * 30)::int
                                        ELSE 1 + (random() * 55000)::int END), '')
             FROM (SELECT random() AS r, i FROM generate_series(1, 1 + (random() * 80)::int)) s) AS d
    FROM generate_series(1, 2000) i;
-- positions are dense, offsets increase and stay inside the document
SELECT count(*) AS bad FROM rnd, LATERAL (
  SELECT max(pos) = count(*) - 1 AND count(DISTINCT pos) = count(*) AS dense,
         bool_and(start_offset < end_offset AND end_offset <= length(d)) AS inside,
         bool_and(octet_length(token) BETWEEN 1 AND 255) AS sized
    FROM sift.tokenize(d)) t
  WHERE NOT (coalesce(dense, true) AND coalesce(inside, true) AND coalesce(sized, true));
-- tokenizing is concatenation independent across a separator
SELECT count(*) AS bad FROM rnd a JOIN rnd b ON b.i = a.i % 2000 + 1
  WHERE toks(a.d || ' ' || b.d) IS DISTINCT FROM nullif(concat_ws(' ', toks(a.d), toks(b.d)), '');
-- tokens are already folded: re-analyzing a token yields itself
SELECT count(*) AS bad FROM rnd, sift.tokenize(d) t WHERE toks(t.token) IS DISTINCT FROM t.token;

-- only NULL selects the default analyzer; a non-NULL zero OID is opened
SELECT token FROM sift.tokenize('Hello', NULL);
SELECT token FROM sift.tokenize('Hello', 0::oid::regclass);

-- the analyzer of a relation that is not a sift index
CREATE TABLE plain (body text);
CREATE INDEX plain_btree ON plain (body);
SELECT * FROM sift.tokenize('x', 'plain_btree');
SELECT * FROM sift.tokenize('x', 'plain');
DROP TABLE plain;

DROP FUNCTION toks;
DROP EXTENSION pg_sift;
