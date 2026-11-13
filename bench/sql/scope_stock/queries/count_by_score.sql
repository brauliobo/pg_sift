-- Events of the members of one tenant counted by score
-- order: 2
--@ events
SELECT score, count(*) FROM {src} WHERE {pred} GROUP BY 1
--@ rollup
SELECT score, sum(n) FROM r_score WHERE {pred} GROUP BY 1
--@ ivm
SELECT score, n FROM im_score WHERE tenant_id = {t}
