-- a fact partitioned by month whose partitions are sift_table (heap for the native engine); an unpartitioned heap dimension
CREATE TABLE source (id int PRIMARY KEY, kind text NOT NULL, region text NOT NULL);
CREATE TABLE event  (id bigint NOT NULL, ts timestamp NOT NULL, feed_id int NOT NULL, value int NOT NULL, status int NOT NULL, PRIMARY KEY (ts, id)) PARTITION BY RANGE (ts);
SELECT format('CREATE TABLE event_%s PARTITION OF event FOR VALUES FROM (%L) TO (%L) USING %s', lpad(m::text, 2, '0'), make_date(2024, m, 1), make_date(2024, m, 1) + interval '1 month', :'table_am')
FROM generate_series(1, 12) m \gexec
