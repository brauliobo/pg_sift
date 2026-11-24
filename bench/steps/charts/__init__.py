"""The README benchmark charts: one module per chart, `light` and `dark` SVGs of each in docs/charts/readme-<name>-<mode>.svg (bench/steps/readme_charts.py runs them).

Output is deterministic: the same results files give the same bytes. Without docs/results the committed baseline of `data.py` is drawn.
"""
import os
from importlib import import_module

from .data import load_live_data
from .theme import MODES

NAMES = ('hero', 'analytics', 'clickbench', 'search', 'scopes', 'scalability', 'storage')


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def render_all(data):
    """{file name: SVG text} of every chart in both modes"""
    out = {}
    for name in NAMES:
        chart = import_module(f'{__name__}.{name}')
        out.update({f'readme-{chart.NAME}-{mode}.svg': chart.render(data, mode) for mode in MODES})
    return out


def generate(root):
    out_dir = os.path.join(root, 'docs', 'charts')
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for name, svg in render_all(load_live_data(os.path.join(root, 'docs', 'results'))).items():
        path = os.path.join(out_dir, name)
        if not os.path.exists(path) or read(path) != svg:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(svg)
        written.append(path)
    return written
