#!/usr/bin/env python3
"""Draw the README benchmark charts of docs/charts (standard library only): bench/steps/charts/ holds one module per chart.

    readme_charts.py [--root DIR]     writes docs/charts/readme-{hero,analytics,clickbench,search,scopes,scalability,storage}-{light,dark}.svg

The numbers come from docs/results/ when its files exist and from the committed baseline of charts/data.py otherwise (a public checkout). A series nobody measured
yet is drawn as `not measured yet`. Output is deterministic: identical inputs give identical bytes.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from charts import generate  # noqa: E402
from charts.data import MEMORY_BASELINE, QUERY_ORDER_ANALYTICS, QUERY_SLUG, load_live_data, read_tsv  # noqa: E402,F401


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=REPO)
    for path in generate(ap.parse_args().root):
        print(path)


if __name__ == '__main__':
    main()
