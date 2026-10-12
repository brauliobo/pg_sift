-- Stand-in for the sift extension, used by scripts/isolation.sh --emulate to
-- validate the specs and their expected output before the features land:
-- index DDL is rewritten to GIN over tsvector (same locks, CIC waits), the
-- operator and checks evaluate through the SQL reference.
CREATE SCHEMA sift;
CREATE FUNCTION sift.match(doc text, q text) RETURNS bool LANGUAGE sql STABLE
AS $$ SELECT sift_ref.match(doc, q) $$;
CREATE OPERATOR @@> (LEFTARG = text, RIGHTARG = text, FUNCTION = sift.match);
CREATE FUNCTION sift.score(tid) RETURNS real LANGUAGE sql AS $$ SELECT 0::real $$;
CREATE FUNCTION sift.promote(regclass) RETURNS int LANGUAGE sql AS $$ SELECT 0 $$;
CREATE FUNCTION sift.merge(regclass) RETURNS int LANGUAGE sql AS $$ SELECT 0 $$;
CREATE FUNCTION sift.verify(regclass, bool DEFAULT false) RETURNS void LANGUAGE plpgsql AS $$ BEGIN END $$;
