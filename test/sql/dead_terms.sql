-- A dictionary keeps the terms of deleted docs until its segment is rewritten. They match
-- nothing, so only terms with a live posting count toward sift.max_expansions, and a forced
-- merge drops them even from a lone segment.
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;
SET enable_indexscan = off;

CREATE TABLE dt (id int, body text);
INSERT INTO dt SELECT i, 'ri' || i || ' base' FROM generate_series(1, 1000) i;
CREATE INDEX dt_idx ON dt USING sift (body);
DELETE FROM dt WHERE id <= 440;
VACUUM dt;
SELECT dirty, nterms FROM sift.segments('dt_idx');

SET sift.max_expansions = 600;
SELECT count(*) FROM dt WHERE body @@> 'ri*';
SELECT id FROM dt WHERE body @@> 'ri*' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SET sift.max_expansions = 500;
SELECT count(*) FROM dt WHERE body @@> 'ri*';

SET sift.max_expansions = 600;
SELECT sift.merge('dt_idx', true);
SELECT dirty, nterms FROM sift.segments('dt_idx');
SELECT count(*) FROM dt WHERE body @@> 'ri*';
SELECT sift.merge('dt_idx', true);
SELECT sift.verify('dt_idx', true);

-- the live terms of several segments are counted once
CREATE TABLE dt2 (id int, body text);
INSERT INTO dt2 SELECT i, 'ri' || i FROM generate_series(1, 400) i;
CREATE INDEX dt2_idx ON dt2 USING sift (body);
DELETE FROM dt2 WHERE id <= 150;
VACUUM dt2;
INSERT INTO dt2 SELECT i, 'ri' || i FROM generate_series(401, 800) i;
SELECT sift.promote('dt2_idx') > 0 AS promoted;
SELECT count(*) AS segments, count(*) FILTER (WHERE dirty) AS dirty FROM sift.segments('dt2_idx') WHERE kind = 'immutable';
SET sift.max_expansions = 700;
SELECT count(*) FROM dt2 WHERE body @@> 'ri*';
SET sift.max_expansions = 500;
SELECT count(*) FROM dt2 WHERE body @@> 'ri*';
SELECT sift.verify('dt2_idx', true);

DROP TABLE dt, dt2;
DROP EXTENSION pg_sift;
