\set ECHO none
-- Shared by the agg tests that compare the rows of a query with the aggregate scan off and on (\ir test/agg_same_helpers.sql, after test/agg_helpers.sql):
-- same(q) is 'same' when runq(q, false) and runq(q, true) end alike (the rows or the error), else both outcomes cut to 200 characters.
CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;
\set ECHO all
