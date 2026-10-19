SELECT count(*) FROM docs d WHERE d.items @> '[{"kind": "c3", "flag": "fa"}]'
