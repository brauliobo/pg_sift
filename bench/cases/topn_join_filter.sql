-- a top-N over an inner join to a table that only filters runs in the column scan
-- label: `SELECT e.ts FROM events e JOIN dims d .. WHERE d.tier = 'tier_a' ORDER BY e.ts DESC LIMIT 10`
-- ordered: yes
--@ pg
SELECT e.ts FROM events e JOIN dims d ON d.id = e.parent_id WHERE d.tier = 'tier_a' ORDER BY e.ts DESC LIMIT 10
