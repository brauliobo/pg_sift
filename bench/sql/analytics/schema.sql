-- The analytics corpus: an orders table of the usual e-commerce ORM shape (one row per order, scalar columns plus jsonb objects and
-- arrays of objects) and the small tables of the queries. Indexes come after the load (indexes.sql).
DROP TABLE IF EXISTS orders, metrics_map, categories_map, kinds_map, analytics_sets, analytics_meta CASCADE;

CREATE TABLE orders (
    id integer NOT NULL,
    store_id integer NOT NULL,
    customer_id integer,
    number text,
    placed_at date,
    status text,
    currency text,
    source text,
    country_code character varying(2),
    lang text,
    total numeric(12,2),
    discount numeric(12,2),
    shipping_method text,
    payment_method text,
    attributes jsonb DEFAULT '{}'::jsonb,   -- object: locale, channel, device, normalized (true when the items carry canonical names)
    metrics jsonb DEFAULT '{}'::jsonb,      -- object with dynamic keys and float values: {"p_total": 4.5, "delivery_speed": 3.0, ...}
    items jsonb DEFAULT '[]'::jsonb,        -- array of objects: [{category, sku, kind, qty, price, discount}]
    notes jsonb DEFAULT '[]'::jsonb,        -- array of objects: [{type, author_role, text}]
    created_at timestamp without time zone DEFAULT now(),
    updated_at timestamp without time zone DEFAULT now(),
    deleted_at timestamp without time zone
);

-- metric key of a source -> metric and part (a key without a row falls back to the split of the key at the underscore)
CREATE TABLE metrics_map (source text NOT NULL, metric_key text NOT NULL, mapped_metric text, mapped_part text);
-- raw item category -> canonical category ('' = not mapped) of the orders whose items are not normalized
CREATE TABLE categories_map (category text NOT NULL, mapped_category text NOT NULL);
-- raw item kind -> canonical kind ('' = none)
CREATE TABLE kinds_map (kind text NOT NULL, mapped_kind text NOT NULL);
-- the member lists of the queries: K10 / K400 / K4000 are hash buckets of the store ids, each inside the next; sources lists the sources
CREATE TABLE analytics_sets (set_name text NOT NULL, member text NOT NULL);
CREATE TABLE analytics_meta (key text PRIMARY KEY, value text);
