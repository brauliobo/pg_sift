-- time series: reading -> device -> site; heap order is time order, devices are random
CREATE TABLE site    (id int PRIMARY KEY, region text NOT NULL, tz text NOT NULL);
CREATE TABLE device  (id int PRIMARY KEY, site_id int NOT NULL, model text NOT NULL, installed_at date NOT NULL);
CREATE TABLE reading (id bigint PRIMARY KEY, device_id int NOT NULL, ts timestamp NOT NULL, value numeric(10,3) NOT NULL, status int NOT NULL);
