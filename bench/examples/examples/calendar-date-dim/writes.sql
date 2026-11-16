-- batch: append-sales
-- pattern: append_only
-- rows: nb
INSERT INTO sale SELECT * FROM gen_sale(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: holidays-change
-- pattern: dim_attr_churn
-- rows: 73
UPDATE calendar SET is_holiday = NOT is_holiday WHERE extract(doy FROM day)::int % 10 = :round % 10;

-- batch: fiscal-quarter-shift
-- pattern: dim_attr_churn
-- rows: 731
UPDATE calendar SET quarter = 'q' || (1 + (month + :round) % 4);

-- batch: sale-change-day
-- pattern: key_churn
-- rows: nf / 50
UPDATE sale SET day = day + 1 WHERE id <= :nf AND id % 50 = :round % 50 AND day < date '2024-12-30';

-- batch: calendar-extend
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: 30
INSERT INTO calendar SELECT * FROM gen_calendar(732, 761) g WHERE NOT EXISTS (SELECT 1 FROM calendar x WHERE x.day = g.day);

-- batch: delete-sales
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM sale WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-sales
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO sale SELECT * FROM gen_sale(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) sale;
REINDEX TABLE CONCURRENTLY sale;
