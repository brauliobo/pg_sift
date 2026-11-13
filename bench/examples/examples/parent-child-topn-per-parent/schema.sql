-- a parent and its children; the queries look from the parent side (EXISTS, NOT EXISTS, IN, LEFT JOIN with a child condition, counts per parent)
CREATE TABLE parent (id int PRIMARY KEY, grp text NOT NULL, state text NOT NULL, rank int NOT NULL, opened date NOT NULL);
CREATE TABLE child  (id bigint PRIMARY KEY, parent_id int, kind text NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL);
