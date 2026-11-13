-- an order document holds an array of items [{sku, qty}]; each element is joined to a catalog by sku (one document, several joined rows)
CREATE TABLE catalog (sku int PRIMARY KEY, category text NOT NULL, brand text NOT NULL, price numeric(10,2) NOT NULL);
CREATE TABLE doc     (id bigint PRIMARY KEY, channel text NOT NULL, items jsonb NOT NULL, at timestamp NOT NULL);
