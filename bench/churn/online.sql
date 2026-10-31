-- writers for waste.sh online: UPDATE-by-id of an indexed column, of a plain column and of the text, a DELETE and an INSERT of a new id (the ids above :n are new)
\set n :rows
\set id random(1, :n)
\set which random(1, 10)
\if :which <= 4
UPDATE :tbl SET value_num = value_num + 1, updated_at = now() WHERE id = :id;
\elif :which <= 6
UPDATE :tbl SET counter_a = counter_a + 1 WHERE id = :id;
\elif :which <= 8
UPDATE :tbl SET body = 'w' || (:id % 2000) || ' ' || body WHERE id = :id;
\elif :which = 9
DELETE FROM :tbl WHERE id = :id;
\else
INSERT INTO :tbl SELECT :n + 1 + abs(hashint8(random(1, 1000000000))), :id % 20, :id % 5000, 'kind_' || (:id % 8), 1, 0, now(), 'w1 w2 w3', '{}' ON CONFLICT DO NOTHING;
\endif
