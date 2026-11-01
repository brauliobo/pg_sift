"""python3 -I bench/steps/records check|report [--tsv] FILE...   (bench/bench records ...)"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402
import report  # noqa: E402


def main(argv):
    if len(argv) < 2 or argv[0] not in ('check', 'report'):
        sys.exit(__doc__)
    return {'check': check.main, 'report': report.main}[argv[0]](argv[1:])


sys.exit(main(sys.argv[1:]))
