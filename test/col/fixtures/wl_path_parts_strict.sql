-- trap: a strict path raises for a document without the key: the heap raises, the scan must not answer instead
-- shape: trap
-- expect: plain
-- key: -
-- error: yes
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), 'strict $.parts[*] ? (@.tone == "up")')) AS n
FROM t_work w
