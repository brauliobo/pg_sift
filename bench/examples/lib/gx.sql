-- Deterministic generator helpers shared by every example (schema gx). Plain SQL, no external files: a row is a pure function of
-- (row number, salt), so a scale, a knob setting and a seed always produce the same table. Examples write their salts as `:s + N`
-- (psql variable s = seed * 1000, N a small constant per column), so one seed changes every column at once.
--
--   gx.u(i, salt)                      uniform value in [0, 1)
--   gx.pick(i, salt, n, skew)          integer in 1..n; skew = 1 uniform, > 1 power law toward 1 (the "zipf" knob)
--   gx.isnull(i, salt, pct)            true for pct percent of the rows
--   gx.fkey(i, nf, nd, salt, corr, skew, hot_pct)
--                                      the join key of fact row i into a dimension of nd rows:
--                                      corr = random     key independent of the heap position
--                                             clustered  key grows with the row number (heap order = key order)
--                                             anti       key falls with the row number
--                                      hot_pct percent of the rows take key 1 (a hot key), whatever the other knobs say
--   gx.word(i, salt, card, skew)       text 'w<k>' with k in 1..card (a dimension attribute of a chosen cardinality)
--   gx.ts(i, salt, start, days)        timestamp inside [start, start + days)
CREATE SCHEMA IF NOT EXISTS gx;

CREATE OR REPLACE FUNCTION gx.u(i bigint, salt int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT (hashint8extended(i, salt::bigint * 1000003 + 7) & 281474976710655)::float8 / 281474976710656.0 $$;

CREATE OR REPLACE FUNCTION gx.pick(i bigint, salt int, n int, skew float8 DEFAULT 1) RETURNS int LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT 1 + least(n - 1, floor(n * power(gx.u(i, salt), skew)))::int $$;

CREATE OR REPLACE FUNCTION gx.isnull(i bigint, salt int, pct float8) RETURNS bool LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT gx.u(i, salt) * 100 < pct $$;

CREATE OR REPLACE FUNCTION gx.fkey(i bigint, nf bigint, nd int, salt int, corr text, skew float8, hot_pct float8)
RETURNS int LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT CASE
       WHEN gx.u(i, salt + 1) * 100 < hot_pct THEN 1
       WHEN corr = 'clustered' THEN 1 + least(nd - 1, (i - 1) * nd / nf)::int
       WHEN corr = 'anti' THEN nd - least(nd - 1, (i - 1) * nd / nf)::int
       ELSE gx.pick(i, salt, nd, skew) END $$;

CREATE OR REPLACE FUNCTION gx.word(i bigint, salt int, card int, skew float8 DEFAULT 1) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT 'w' || gx.pick(i, salt, card, skew) $$;

CREATE OR REPLACE FUNCTION gx.ts(i bigint, salt int, start timestamp, days int) RETURNS timestamp LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT start + (gx.u(i, salt) * days * 86400)::bigint * interval '1 second' $$;
