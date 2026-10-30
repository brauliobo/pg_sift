# The constants shared by the charts of the README benchmarks, the wide canvas and the dot of a system.
# It also adds pg_duckdb to the systems of store_charts.

import store_charts as sc
from store_charts import Canvas, SYSTEMS, num, tw


SYSTEMS['duckdb'] = ('pg_duckdb', '#eb6834', '#d95926', 'up')
ORDER = ['pg', 'sift', 'sift_table', 'es', 'ch', 'citus', 'duckdb']
SHORT = dict(sc.SHORT, duckdb='pg_duckdb', es='Elasticsearch')
HEADER = dict(SHORT, es='Elastic', sift='sift_col', sift_table='sift_table')   # the column heads of a heat strip are narrower
W = 1000


class Wide(Canvas):
    """a Canvas that is W pixels wide (the old charts are 880)"""

    def legend(self, x, y, sids, labels=None):
        for sid in sids:
            label = (labels or {}).get(sid) or SYSTEMS[sid][0]
            if x + 18 + tw(label, 13) > W - 20:
                x, y = 20, y + 22
            self.marker(x + 5, y - 4, sid)
            self.text(x + 16, y, label, 13)
            x += 16 + tw(label, 13) + 18
        return y

    def head(self, title, sub):
        self.text(20, 32, title, 18, weight='bold')
        for i, line in enumerate(self.wrap(sub, 13, W - 40)):
            self.text(20, 54 + 18 * i, line, 13, fill=self.t['muted'])
        self.sub_lines = len(self.wrap(sub, 13, W - 40))

    def wrap(self, s, size, width):
        out, cur = [], ''
        for w in s.split(' '):
            if cur and tw(cur + ' ' + w, size) > width:
                out.append(cur)
                cur = w
            else:
                cur = (cur + ' ' + w).strip()
        return out + [cur]

    def foot(self, y, lines):
        n = 0
        for s in lines:
            for line in self.wrap(s, 12, W - 40):
                self.text(20, y + 17 * n, line, 12, fill=self.t['muted'])
                n += 1
        return y + 17 * n

    def render(self, height, title, desc):
        bg, border = self.t['bg'], self.t['border']
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{height}" viewBox="0 0 {W} {height}" role="img" aria-labelledby="t d" {sc.FONT}>\n'
                f'<title id="t">{sc.escape(title)}</title>\n<desc id="d">{sc.escape(desc)}</desc>\n<rect x="0.5" y="0.5" width="{W - 1}" height="{height - 1}" rx="6" fill="{bg}" stroke="{border}"/>\n'
                + '\n'.join(self.parts) + '\n</svg>\n')

    def text_box(self, x, y, s, size=12, fill=None, weight=None, anchor='start'):
        """a text on a surface-coloured plate, so a label over a grid line stays readable"""
        w = tw(s, size) + 6
        x0 = x - w / 2 if anchor == 'middle' else x - w + 3 if anchor == 'end' else x - 3
        self.rect(x0, y - size, w, size + 5, self.t['bg'], 3)
        self.text(x, y, s, size, anchor, fill, weight)


def dot(c, x, y, faster, title, r=6):
    """a dot in the colour of the outcome (blue: faster, orange: slower), ringed with the surface colour: no system owns it"""
    fill = c.t['up'] if faster else c.t['down']
    t = f'<title>{sc.escape(title)}</title>'
    shape = (f'<circle cx="{num(x)}" cy="{num(y)}" r="{r}" fill="{fill}" stroke="{c.t["bg"]}" stroke-width="2" paint-order="stroke">{t}</circle>' if faster else
             f'<polygon points="{num(x)},{num(y - r * 1.3)} {num(x + r * 1.2)},{num(y + r)} {num(x - r * 1.2)},{num(y + r)}" fill="{fill}" stroke="{c.t["bg"]}" stroke-width="2" paint-order="stroke">{t}</polygon>')
    c.parts.append(shape)
