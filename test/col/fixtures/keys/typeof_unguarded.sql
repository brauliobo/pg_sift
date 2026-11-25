-- typeof: a sum over the leaf that no type conjunct guards still raises (a string where a number is read), in the scan and in PostgreSQL
-- shape: trap
-- expect: any
-- error: either
-- key: -
SELECT sum((attributes->>'weight')::float8) FROM orders
