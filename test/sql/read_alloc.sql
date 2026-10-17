-- What the read side allocates per query: a cursor over a sparse term decodes into a buffer the size of its
-- rows, not the 32 kB of a group of a frequent term, so a leaf with hundreds of terms over many segments
-- does not allocate hundreds of times 32 kB per segment (sift._test_read_stats counts the bytes).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE ra (id serial, body text);
CREATE INDEX ra_idx ON ra USING sift (body);
-- six immutable segments, each with the 300 words and a frequent one
DO $$
BEGIN
	FOR i IN 1..6 LOOP
		INSERT INTO ra (body)
		SELECT 'common w' || (g % 300) || ' x' || (g % 17) || ' y' || (g % 1000) FROM generate_series(1, 4000) g;
		PERFORM sift.promote('ra_idx');
	END LOOP;
END $$;
SELECT count(*) AS segments FROM sift.segments('ra_idx'::regclass) WHERE kind = 'immutable';

CREATE FUNCTION ra_q(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM ra WHERE body @@> $1' INTO n USING q;
	RETURN n;
END $$;

-- a 200-term OR: no term is frequent enough to be a GROUPED blob (8192 postings), every cursor is sparse
SELECT string_agg('w' || g, ' OR ') AS q FROM generate_series(0, 199) g \gset
SELECT ra_q(:'q') AS matches;
SELECT count(*) AS reset FROM sift._test_read_stats(true);
SELECT ra_q(:'q') AS matches;
SELECT value AS cursors FROM sift._test_read_stats() WHERE key = 'cursors' \gset
SELECT value AS bytes FROM sift._test_read_stats() WHERE key = 'cursor_buffer_bytes' \gset
SELECT :cursors >= 6 * 200 AS cursor_per_term_and_segment, :bytes / :cursors < 4096 AS sparse_buffer_under_4kB,
	round(:bytes / :cursors / 32768.0, 2) < 0.1 AS under_a_tenth_of_a_group_buffer;

DROP FUNCTION ra_q(text);
DROP TABLE ra;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
