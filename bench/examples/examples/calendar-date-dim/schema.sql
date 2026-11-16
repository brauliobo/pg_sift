-- a calendar dimension keyed by date: facts carry a day, the calendar says weekday, month, quarter and holiday
CREATE TABLE calendar (day date PRIMARY KEY, dow int NOT NULL, month int NOT NULL, quarter text NOT NULL, is_holiday boolean NOT NULL);
CREATE TABLE sale     (id bigint PRIMARY KEY, day date NOT NULL, store int NOT NULL, amount numeric(12,2) NOT NULL);
