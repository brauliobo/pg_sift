-- A selective filter: the events of one channel and one region of the members of one tenant
-- order: 7
--@ events
SELECT count(*) FROM {src} WHERE {pred} AND channel_id = 9 AND region = 'DE'
--@ rollup
SELECT coalesce(sum(n), 0) FROM r_chan WHERE {pred} AND channel_id = 9 AND region = 'DE'
--@ ivm
SELECT coalesce(sum(n), 0) FROM im_chan WHERE tenant_id = {t} AND channel_id = 9 AND region = 'DE'
