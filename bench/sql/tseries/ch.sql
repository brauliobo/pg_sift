CREATE TABLE metrics (ts DateTime64(6), device Int32, metric Int16, value Float64) ENGINE = MergeTree ORDER BY ts
