-- trap: a strict path raises for a document without the key: the heap raises, the scan must not answer instead
-- shape: trap
-- expect: plain
-- key: -
-- error: yes
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), 'strict $.parts[*] ? (@.polarity == "positive")')) AS n
FROM t_load w
