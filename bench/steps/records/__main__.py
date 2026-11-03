"""python3 -I bench/steps/records check|report|claims [--tsv] FILE...   (bench/bench records ...)

    migrate FILE...    write each file of an older schema again in the current one (the old file stays as FILE.schemaN)"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402
import claims  # noqa: E402
import report  # noqa: E402
import schema  # noqa: E402


def main(argv):
    if len(argv) < 2 or argv[0] not in ('check', 'report', 'claims', 'migrate'):
        sys.exit(__doc__)
    if argv[0] == 'migrate':
        for f in argv[1:]:
            old = schema.migrate(f)
            print('%s: %s' % (f, 'already schema %d' % schema.VERSION if old is None else 'schema %d -> %d (the old file is %s.schema%d)' % (old, schema.VERSION, f, old)))
        return 0
    return {'check': check.main, 'report': report.main, 'claims': claims.main}[argv[0]](argv[1:])


sys.exit(main(sys.argv[1:]))
