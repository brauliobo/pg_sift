"""Palette, SVG canvas, number formats and the label-overlap guard shared by every chart of docs/charts (standard library only).

Every chart is drawn twice, `light` and `dark`, on a transparent canvas: the README picks one with <picture>. pg_sift is blue (`sift_col`) and emerald (`sift_table`),
every rival is slate, and amber marks only a place where a rival beats pg_sift. A canvas refuses to write an SVG whose text boxes overlap or leave the canvas.
"""
import math
from xml.sax.saxutils import escape

WIDTH = 860
LEFT, RIGHT = 24, WIDTH - 24
FONT = '-apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif'
MODES = ('light', 'dark')
PALETTE = {
    'light': dict(text='#1f2328', sub='#57606a', grid='#d8dee4', axis='#8c959f', col='#2563EB', tbl='#059669', rival='#64748b', soft='#94a3b8', amber='#D97706'),
    'dark':  dict(text='#e6edf3', sub='#8b949e', grid='#30363d', axis='#6e7681', col='#3B82F6', tbl='#10B981', rival='#94a3b8', soft='#64748b', amber='#F59E0B'),
}
ENGINE_ROLE = {'sift': 'col', 'sift_col': 'col', 'sift_scope': 'col', 'sift_table': 'tbl'}
NARROW, WIDE = set("iljtf.,:;|!'()[]/- ·×"), set('mwMW@%—')


def role(engine):
    return ENGINE_ROLE.get(engine, 'rival')


def n(v):
    s = f'{v:.1f}'
    return s[:-2] if s.endswith('.0') else s


def text_width(s, size, bold=False):
    w = sum(.3 if c in NARROW else .86 if c in WIDE else .66 if c.isupper() else .56 if c.isdigit() else .54 for c in s) * size
    return w * 1.07 if bold else w


def short(v):
    return f'{v:,.0f}' if v >= 100 else f'{v:.1f}' if v >= 10 else f'{v:.2f}'


def fmt_ms(v):
    return short(v) + ' ms'


def fmt_qps(v):
    return f'{v / 1000:.1f}k' if v >= 10000 else f'{v:,.0f}'


def fmt_mib(v):
    return f'{v:,.0f} MiB' if v >= 100 else f'{v:.1f} MiB'


def fmt_x(v):
    return f'{v:,.0f}×' if v >= 100 else f'{v:.1f}×' if v < 10 else f'{v:.0f}×'


def geomean(xs):
    return math.exp(sum(math.log(max(x, 1e-6)) for x in xs) / len(xs))


def scale(lo, hi, a, b, log=False):
    f = math.log10 if log else float
    return lambda v: a + (f(min(max(v, lo), hi)) - f(lo)) / (f(hi) - f(lo)) * (b - a)


class Canvas:
    def __init__(self, mode, height):
        self.mode, self.height, self.p, self.parts, self.boxes = mode, height, PALETTE[mode], [], []

    def color(self, name):
        return 'none' if name is None else self.p.get(name, name)

    def attrs(self, fill=None, stroke=None, width=None, dash=None, opacity=None, **extra):
        a = {'fill': self.color(fill), 'stroke': stroke and self.color(stroke), 'stroke-width': width and n(width), 'stroke-dasharray': dash, 'opacity': opacity, **extra}
        return ''.join(f' {k}="{v}"' for k, v in a.items() if v is not None)

    def text(self, x, y, s, fill='text', size=12, anchor='start', bold=False):
        w = text_width(s, size, bold)
        x0 = {'start': x, 'middle': x - w / 2, 'end': x - w}[anchor]
        self.boxes.append((x0, y - size * .8, x0 + w, y + size * .25, s))
        weight = ' font-weight="700"' if bold else ''
        self.parts.append(f'<text x="{n(x)}" y="{n(y)}" font-size="{size}"{weight} text-anchor="{anchor}"{self.attrs(fill)}>{escape(s)}</text>')

    def rect(self, x, y, w, h, fill, r=0, opacity=None, stroke=None, width=1):
        rr = f' rx="{n(r)}"' if r else ''
        self.parts.append(f'<rect x="{n(x)}" y="{n(y)}" width="{n(w)}" height="{n(h)}"{rr}{self.attrs(fill, stroke, stroke and width, opacity=opacity)}/>')

    def line(self, x1, y1, x2, y2, stroke='grid', width=1, dash=None):
        self.parts.append(f'<line x1="{n(x1)}" y1="{n(y1)}" x2="{n(x2)}" y2="{n(y2)}"{self.attrs(None, stroke, width, dash)}/>')

    def circle(self, x, y, r, fill, stroke=None, width=1.5):
        self.parts.append(f'<circle cx="{n(x)}" cy="{n(y)}" r="{n(r)}"{self.attrs(fill, stroke, stroke and width)}/>')

    def poly(self, pts, stroke, width=2, dash=None):
        p = ' '.join(f'{n(x)},{n(y)}' for x, y in pts)
        self.parts.append(f'<polyline points="{p}"{self.attrs(None, stroke, width, dash, **{"stroke-linejoin": "round", "stroke-linecap": "round"})}/>')

    def header(self, title, sub):
        self.text(LEFT, 30, title, size=14, bold=True)
        self.text(LEFT, 50, sub, 'sub')

    def footer(self, s, y=None):
        self.text(LEFT, y or self.height - 12, s, 'sub')

    def legend(self, x, y, items, anchor='start'):
        """items: (role or engine, label); a swatch and its label, left to right from x (or ending at x)"""
        widths = [16 + text_width(label, 12) + 18 for _, label in items]
        cx = x - sum(widths) + 18 if anchor == 'end' else x
        for (name, label), w in zip(items, widths):
            self.rect(cx, y - 9, 11, 11, name if name in self.p else role(name), r=2.5)
            self.text(cx + 16, y, label)
            cx += w

    def unmeasured(self, x, y, s, anchor='middle'):
        self.text(x, y, s, 'sub', anchor=anchor)

    def check(self):
        for i, a in enumerate(self.boxes):
            if a[0] < 0 or a[2] > WIDTH or a[1] < 0 or a[3] > self.height:
                raise ValueError(f'label leaves the canvas: {a[4]!r}')
            for b in self.boxes[i + 1:]:
                if min(a[2], b[2]) - max(a[0], b[0]) > 1 and min(a[3], b[3]) - max(a[1], b[1]) > 1:
                    raise ValueError(f'labels overlap: {a[4]!r} and {b[4]!r}')

    def svg(self, title, desc):
        self.check()
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {self.height}" width="{WIDTH}" height="{self.height}" role="img" '
                f'aria-labelledby="t d" font-family=\'{FONT}\'>\n<title id="t">{escape(title)}</title>\n<desc id="d">{escape(desc)}</desc>')
        return head + '\n' + '\n'.join(self.parts) + '\n</svg>\n'
