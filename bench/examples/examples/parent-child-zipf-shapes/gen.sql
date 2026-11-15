-- the last 10% of the parents (at least one) have no child; null_pct percent of the children have a NULL parent_id; hot percent take parent 1
CREATE FUNCTION gen_parent(lo bigint, hi bigint) RETURNS SETOF parent LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, k.card), 'w' || gx.pick(g, k.s + 2, 5), gx.pick(g, k.s + 3, 100), date '2020-01-01' + gx.pick(g, k.s + 4, 1000)
  FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_child(lo bigint, hi bigint) RETURNS SETOF child LANGUAGE sql AS $$
  SELECT g, CASE WHEN gx.isnull(g, k.s + 10, k.null_pct) THEN NULL ELSE gx.fkey(g, k.nf, greatest(k.nd * 9 / 10, 1), k.s + 11, k.corr, k.skew, k.hot) END,
         (ARRAY['a', 'b', 'c', 'd'])[gx.pick(g, k.s + 12, 4)], gx.pick(g, k.s + 13, 100000) / 100.0, gx.ts(g, k.s + 14, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO parent SELECT * FROM gen_parent(1, :nd);
INSERT INTO child SELECT * FROM gen_child(1, :nf);
