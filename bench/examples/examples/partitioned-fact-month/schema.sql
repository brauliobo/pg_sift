-- a fact partitioned by month (2024) joined to an unpartitioned dimension; the partition key is part of the primary key
CREATE TABLE source (id int PRIMARY KEY, kind text NOT NULL, region text NOT NULL);
CREATE TABLE event  (id bigint NOT NULL, ts timestamp NOT NULL, feed_id int NOT NULL, value int NOT NULL, status int NOT NULL, PRIMARY KEY (ts, id)) PARTITION BY RANGE (ts);
DO $$ BEGIN
  FOR m IN 1..12 LOOP
    EXECUTE format('CREATE TABLE event_%s PARTITION OF event FOR VALUES FROM (%L) TO (%L)', lpad(m::text, 2, '0'), make_date(2024, m, 1), make_date(2024, m, 1) + interval '1 month');
  END LOOP;
END $$;
