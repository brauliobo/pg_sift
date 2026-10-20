-- trap: a strict path raises for a document without the key: the heap raises, the scan must not answer instead
-- shape: trap
-- expect: plain
-- key: -
-- error: yes
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.wl_norm(w.doc), 'strict $.opinions[*] ? (@.polarity == "positive")')) AS n
FROM t_wl w
