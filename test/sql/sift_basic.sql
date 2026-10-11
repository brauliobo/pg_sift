CREATE EXTENSION sift;
SELECT amname, amtype FROM pg_am WHERE amname = 'sift';
SELECT oprnamespace::regnamespace, oprname, oprleft::regtype, oprright::regtype
  FROM pg_operator WHERE oprname = '@@>';
SELECT opcname, opcdefault FROM pg_opclass
  WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'sift');
CREATE TABLE docs (id int, body text);
CREATE INDEX docs_body_sift ON docs USING sift (body) WITH (b = 2);
CREATE INDEX docs_body_sift ON docs USING sift (body) WITH (k1 = 1.5, stemmer = 'english_stem');
SET sift.simd = scalar;
SHOW sift.simd;
RESET sift.simd;
DROP TABLE docs;
DROP EXTENSION sift;
