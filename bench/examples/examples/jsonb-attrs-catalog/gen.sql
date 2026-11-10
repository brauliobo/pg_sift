-- catalog = nd = nf/20; events carry {sku, qty, channel, tags}; 5% of the events name a sku that is not in the catalog and the last 10% of the catalog is never sold
CREATE FUNCTION gen_event(lo bigint, hi bigint) RETURNS SETOF event LANGUAGE sql AS $$
  SELECT g, jsonb_build_object('sku', CASE WHEN gx.u(g, k.s + 16) < 0.05 THEN k.nd + 1 + gx.pick(g, k.s + 17, 100) ELSE gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot) END, 'qty', gx.pick(g, k.s + 11, 9), 'channel', 'ch' || gx.pick(g, k.s + 12, 4),
                               'tags', jsonb_build_array('t' || gx.pick(g, k.s + 13, 20), 't' || gx.pick(g, k.s + 14, 20))), gx.ts(g, k.s + 15, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO catalog SELECT g, gx.word(g, :s + 1, :card), 'b' || gx.pick(g, :s + 2, 30), 1 + gx.pick(g, :s + 3, 10000) / 100.0 FROM generate_series(1, :nd) g;
INSERT INTO event SELECT * FROM gen_event(1, :nf);
