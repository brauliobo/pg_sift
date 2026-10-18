-- live_stats: a segment with vacuumed documents scores as the merged segment of its live documents does (N, df and avgdl
-- count live documents only). Each query is scored over the dirty index, then again after the merge that drops the dead
-- documents, and the rows and scores must be equal.
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;

CREATE TABLE ls (id serial PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
SELECT setseed(0.3);
INSERT INTO ls (body) SELECT (SELECT string_agg('w' || floor(300 * power(random(), 3))::int, ' ') FROM generate_series(1, 5 + (i % 40)))
	FROM generate_series(1, 20000) i;
CREATE INDEX ls_idx ON ls USING sift (body);

CREATE TABLE ls_q (n serial, q text);
INSERT INTO ls_q (q) VALUES
	('w5'), ('w5 w50'), ('w5 OR w50 OR w7 OR w100'), ('w5 NOT w7'), ('w1*'), ('w12~1'), ('"w5 w7"'), ('w5 OR "w1 w2"'), ('missing OR w9');

CREATE TABLE ls_s (tag text, n int, id int, s real);
CREATE FUNCTION ls_scores(tag text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	c record;
BEGIN
	FOR c IN SELECT * FROM ls_q LOOP
		EXECUTE format('CREATE TEMP TABLE ls_t AS SELECT id, sift.score(ctid) s FROM ls WHERE body @@> %L', c.q);
		INSERT INTO ls_s SELECT tag, c.n, id, s FROM ls_t;
		DROP TABLE ls_t;
	END LOOP;
END $$;

CREATE FUNCTION ls_compare(a text, b text) RETURNS TABLE (q text, matched bool, same_rows bool, differ bigint) LANGUAGE sql AS $$
	SELECT q.q, count(*) > 0,
		(SELECT count(*) FROM ls_s WHERE tag = a AND n = q.n) = (SELECT count(*) FROM ls_s WHERE tag = b AND n = q.n),
		count(*) FILTER (WHERE x.s <> y.s)
	FROM ls_q q JOIN ls_s x ON x.tag = a AND x.n = q.n JOIN ls_s y ON y.tag = b AND y.n = q.n AND y.id = x.id
	GROUP BY q.n, q.q ORDER BY q.n
$$;

-- vacuumed documents in a built segment
DELETE FROM ls WHERE random() < 0.3 OR id % 7 = 0 AND id < 5000;
VACUUM ls;
SELECT dirty, dead_docs > 5000 AS dead FROM sift.segments('ls_idx');
SELECT sift.verify('ls_idx', true);
SELECT ls_scores('dirty');
SELECT sift.merge('ls_idx', true);
SELECT dirty, dead_docs FROM sift.segments('ls_idx');
SELECT ls_scores('merged');
SELECT * FROM ls_compare('dirty', 'merged');

-- more rows, deleted ones in the mutable segment and in the vacuumed one, and a promoted segment beside them
INSERT INTO ls (body) SELECT (SELECT string_agg('w' || floor(300 * power(random(), 3))::int, ' ') FROM generate_series(1, 5 + (i % 40)))
	FROM generate_series(1, 3000) i;
DELETE FROM ls WHERE id > 20000 AND id % 5 = 0;
SELECT sift.promote('ls_idx') > 0 AS promoted;
INSERT INTO ls (body) SELECT (SELECT string_agg('w' || floor(300 * power(random(), 3))::int, ' ') FROM generate_series(1, 5 + (i % 40)))
	FROM generate_series(1, 1000) i;
DELETE FROM ls WHERE id % 11 = 3;
VACUUM ls;
DELETE FROM ls WHERE id % 13 = 4;
SELECT ls_scores('mixed');
SELECT sift.verify('ls_idx', true);
SELECT sift.promote('ls_idx') > 0 AS promoted;
SELECT sift.merge('ls_idx', true) > 0 AS merged;
SELECT count(*) FILTER (WHERE kind = 'immutable') = 1 AS one_segment, count(*) FILTER (WHERE dirty) AS dirty_left FROM sift.segments('ls_idx');
SELECT ls_scores('final');
SELECT * FROM ls_compare('mixed', 'final');

DROP FUNCTION ls_scores(text), ls_compare(text, text);
DROP TABLE ls, ls_q, ls_s;
DROP EXTENSION pg_sift;
