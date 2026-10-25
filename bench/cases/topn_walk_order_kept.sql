-- a top-N over a sort column that does not follow the table: the order of the walk over the units (their zones sorted once) and the head bypass of the superblocks are kept by the backend, so the first statement of a backend pays for them and the second does not
-- label: `ORDER BY amount DESC LIMIT 20`, the first statement of a new backend and the second
-- ordered: yes
-- fresh: yes
--@ pg
SELECT id, amount FROM orders ORDER BY amount DESC, id LIMIT 20
