-- The shredder a table keeps for its declared paths gives back, at the next document, the payloads of a document whose values its leaves could not hold:
-- a wide string where an int4 is declared, and a list of many strings where int4 elements are declared (a document of small values keeps under 1 MiB)
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE FUNCTION shred_bytes() RETURNS bigint LANGUAGE sql AS
$$ SELECT coalesce(sum(total_bytes), 0)::bigint FROM pg_backend_memory_contexts WHERE name = 'sift cache shred' $$;

CREATE TABLE wide (id int, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('wide', 'doc', 'doc->a int4, doc->b[] int4');
INSERT INTO wide VALUES (0, '{"a": 1, "b": [1]}');
SELECT shred_bytes() BETWEEN 1 AND 1024 * 1024 AS small_after_a_small_document;

INSERT INTO wide VALUES (1, jsonb_build_object('a', repeat('x', 4 << 20)));
SELECT shred_bytes() > 4 << 20 AS holds_the_wide_payload_while_it_is_the_last;
INSERT INTO wide VALUES (2, '{"a": 2}');
SELECT shred_bytes() <= 1024 * 1024 AS small_after_a_wide_payload;

INSERT INTO wide SELECT 3, jsonb_build_object('b', jsonb_agg(repeat('s', 250) || i)) FROM generate_series(1, 8000) i;
INSERT INTO wide VALUES (4, '{"b": [4]}');
SELECT shred_bytes() <= 1024 * 1024 AS small_after_many_payloads;

SELECT id, doc->>'a' IS NOT NULL AS a, jsonb_array_length(doc->'b') AS b FROM wide ORDER BY id;
DROP TABLE wide;
DROP FUNCTION shred_bytes();
DROP EXTENSION pg_sift;
