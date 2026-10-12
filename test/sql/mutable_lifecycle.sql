-- Inserts land in mutable segments, promotion and merge turn them into immutable ones,
-- VACUUM clears liveness bits; verify checks every stage. The maintenance worker may promote
-- on its own, so segment counts are only checked after merge.
CREATE EXTENSION sift;
CREATE TABLE ml (id serial PRIMARY KEY, body text);
CREATE INDEX ml_idx ON ml USING sift (body) WITH (mutable_limit = 64);

CREATE FUNCTION ml_state() RETURNS TABLE (alpha bigint, late bigint, huge bigint, extra bigint) LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE body @@> 'alpha'), count(*) FILTER (WHERE body @@> 'late'),
		   count(*) FILTER (WHERE body @@> 'bigword2999'), count(*) FILTER (WHERE body @@> 'extra')
	FROM ml
$$;
SET enable_seqscan = off;
SET enable_indexscan = off;

-- the large doc spans many mutable pages
INSERT INTO ml (body) SELECT 'alpha beta word' || (i % 50) || ' gamma' FROM generate_series(1, 2000) i;
INSERT INTO ml (body) SELECT string_agg('bigword' || g, ' ') FROM generate_series(1, 3000) g;
SELECT * FROM ml_state();
SELECT sift.verify('ml_idx', true);

SELECT sift.promote('ml_idx') IS NOT NULL AS promoted;
SELECT * FROM ml_state();
SELECT sift.verify('ml_idx', true);

-- deletes and updates, then VACUUM
DELETE FROM ml WHERE id % 3 = 0;
UPDATE ml SET body = body || ' extra' WHERE id % 5 = 0;
SELECT * FROM ml_state();
VACUUM ml;
SELECT * FROM ml_state();
SELECT sift.verify('ml_idx', true);

INSERT INTO ml (body) SELECT 'alpha late' FROM generate_series(1, 300);
SELECT * FROM ml_state();
SELECT sift.promote('ml_idx') IS NOT NULL AS promoted;
SELECT sift.merge('ml_idx', true) IS NOT NULL AS merged;
SELECT count(*) FILTER (WHERE kind = 'immutable') AS immutable, sum(ndocs) FILTER (WHERE kind = 'immutable') AS ndocs
FROM sift.segments('ml_idx');
SELECT * FROM ml_state();
SELECT sift.verify('ml_idx', true);

DROP TABLE ml;
DROP FUNCTION ml_state();
DROP EXTENSION sift;
