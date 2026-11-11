-- catalog = nd = nf/20 skus; a document has 1 to 4 items drawn with the skew knob; 3% of the items name a sku that is not in the catalog; the last 10% of the skus are never sold
CREATE FUNCTION gen_doc(lo bigint, hi bigint) RETURNS SETOF doc LANGUAGE sql AS $$
  SELECT g, 'ch' || gx.pick(g, k.s + 10, 4),
         (SELECT jsonb_agg(jsonb_build_object('sku', CASE WHEN gx.u(g * 8 + j, k.s + 11) < 0.03 THEN k.nd + 1 + gx.pick(g * 8 + j, k.s + 12, 100)
                                                          ELSE gx.pick(g * 8 + j, k.s + 13, greatest(k.nd * 9 / 10, 1), k.skew) END, 'qty', gx.pick(g * 8 + j, k.s + 14, 9)))
          FROM generate_series(1, 1 + gx.pick(g, k.s + 15, 4) - 1) j),
         gx.ts(g, k.s + 16, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO catalog SELECT g, gx.word(g, :s + 1, :card), 'b' || gx.pick(g, :s + 2, 30), 1 + gx.pick(g, :s + 3, 10000) / 100.0 FROM generate_series(1, :nd) g;
INSERT INTO doc SELECT * FROM gen_doc(1, :nf);
