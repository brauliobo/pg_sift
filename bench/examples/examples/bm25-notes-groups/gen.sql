-- groups = nd = nf/20; title = 4 words, body = 16 words of a 400-word vocabulary 'w<k>' drawn with the skew knob; the last 10% of the groups have no note
CREATE FUNCTION words(g bigint, salt int, n int, voc int, sk float8) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
  $$ SELECT string_agg('w' || gx.pick(g * 32 + j, salt, voc, sk), ' ') FROM generate_series(1, n) j $$;
CREATE FUNCTION gen_note(lo bigint, hi bigint) RETURNS SETOF note LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, 1, k.hot), words(g, k.s + 11, 4, 400, k.skew), words(g, k.s + 12, 16, 400, k.skew), gx.ts(g, k.s + 13, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO grp SELECT g, gx.word(g, :s + 1, :card), gx.pick(g, :s + 2, 4) FROM generate_series(1, :nd) g;
INSERT INTO note SELECT * FROM gen_note(1, :nf);
