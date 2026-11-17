-- facts keep their attributes in a jsonb document; the join key is a member of it (attrs->>'sku') into a catalog
CREATE TABLE catalog (sku int PRIMARY KEY, category text NOT NULL, brand text NOT NULL, price numeric(10,2) NOT NULL);
CREATE TABLE event   (id bigint PRIMARY KEY, attrs jsonb NOT NULL, at timestamp NOT NULL);
